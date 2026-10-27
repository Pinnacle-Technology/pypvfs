"""Regression coverage for the native wrapper and staged database saves."""

import ctypes
import os
from pathlib import Path
import sqlite3
import struct

import pytest

from pvfs_tools.Core import pvfs_binding as binding
from pvfs_tools.Core.pvfs_binding import PvfsFile, PvfsHighTimeWrapper, PvfsIndexHeaderWrapper
from pvfs_tools.Core.pvfs_data_file import PvfsDataFile


@pytest.fixture(params=[4096, 524288])
def vfs(tmp_path, request):
    instance = PvfsFile.create(str(tmp_path / "recording.pvfs"), request.param)
    try:
        yield instance
    finally:
        instance.close()


def contents(vfs, name):
    handle = vfs.fopen(name)
    try:
        return handle.read(handle.get_file_info().size)
    finally:
        handle.close()


def test_index_header_layout_and_native_round_trip(vfs):
    assert ctypes.sizeof(PvfsHighTimeWrapper) == 16
    assert ctypes.sizeof(PvfsIndexHeaderWrapper) == 56
    assert PvfsIndexHeaderWrapper.startTime.offset == 24
    assert PvfsIndexHeaderWrapper.endTime.offset == 40
    header = PvfsIndexHeaderWrapper(
        0x12345678, 1, 2, 400.0,
        PvfsHighTimeWrapper(1700000000, 0.125),
        PvfsHighTimeWrapper(1700000060, 0.875),
    )
    handle = vfs.fcreate("signal.index")
    try:
        assert handle.write_index_file_header(header) == 0
        assert handle.flush(commit=True) == 0

        class GuardedHeader(ctypes.Structure):
            _fields_ = [("header", PvfsIndexHeaderWrapper), ("guard", ctypes.c_ubyte * 32)]

        result = GuardedHeader()
        result.guard[:] = [0xA5] * 32
        assert binding._get_lib().read_index_file_header(handle.handle, ctypes.byref(result.header)) == 0
        assert bytes(result.guard) == b"\xa5" * 32
        assert result.header.startTime.seconds == header.startTime.seconds
        assert result.header.startTime.subseconds == 0.125
        assert result.header.endTime.seconds == header.endTime.seconds
        assert result.header.endTime.subseconds == 0.875
        assert result.header.datarate == 400.0
        # The on-disk layout is packed independently of the C wrapper ABI.
        handle.seek(0)
        raw = handle.read(52)
        assert struct.unpack("<IIIfqdqdI", raw) == (
            0x12345678, 1, 2, 400.0, 1700000000, 0.125, 1700000060, 0.875, 10
        )
    finally:
        handle.close()


def test_large_round_trip_seek_extract_and_replacement(vfs, tmp_path):
    payload = bytes(range(256)) * 200
    handle = vfs.fcreate("payload")
    try:
        assert handle.write(payload, len(payload)) == len(payload)
        assert handle.flush(commit=True) == 0
        assert handle.seek(4000) == 0
        assert handle.tell() == 4000
        assert handle.read(2000) == payload[4000:6000]
    finally:
        handle.close()
    assert contents(vfs, "payload") == payload
    output = tmp_path / "extracted.bin"
    assert vfs.extract("payload", str(output)) == 0
    assert output.read_bytes() == payload

    for replacement in (payload[::-1] * 2, b"small", b""):
        source = tmp_path / "replacement.bin"
        source.write_bytes(replacement)
        assert vfs.overwrite("payload", source) == 0
        assert contents(vfs, "payload") == replacement
    with pytest.raises(RuntimeError, match="overwrite"):
        vfs.overwrite("payload", tmp_path / "missing.bin")
    assert contents(vfs, "payload") == b""
    # Failure also releases the lock, so a subsequent operation completes.
    assert vfs.extract("payload", str(output)) == 0


def test_extraction_errors(vfs, tmp_path):
    source = tmp_path / "source.bin"
    source.write_bytes(b"test" * 20000)
    vfs.overwrite("payload", source)
    with pytest.raises(RuntimeError, match="extract"):
        vfs.extract("missing", str(tmp_path / "out.bin"))
    with pytest.raises(RuntimeError, match="extract"):
        vfs.extract("payload", str(tmp_path / "missing-dir" / "out.bin"))
    if os.name != "nt" and Path("/dev/full").exists():
        with pytest.raises(RuntimeError, match="extract"):
            vfs.extract("payload", "/dev/full")
    assert contents(vfs, "payload") == source.read_bytes()


def test_shared_readers_and_exclusive_writer(tmp_path):
    path = str(tmp_path / "recording.pvfs")
    writer = PvfsFile.create(path)
    source = tmp_path / "payload.bin"
    source.write_bytes(b"readable while writer is open")
    writer.overwrite("payload", source)
    try:
        with pytest.raises(RuntimeError, match="open"):
            PvfsFile.open(path)
        readers = []
        try:
            for _ in range(2):
                readers.append(PvfsFile.open(path, read_only=True))
                assert contents(readers[-1], "payload") == source.read_bytes()
            with pytest.raises(RuntimeError, match="overwrite"):
                readers[0].overwrite("payload", source)
        finally:
            for reader in readers:
                reader.close()
    finally:
        writer.close()
    reopened = PvfsFile.open(path)
    reopened.close()


def test_database_snapshot_and_read_only_open(tmp_path):
    path = str(tmp_path / "experiment.pvfs")
    data = PvfsDataFile()
    try:
        assert data.create(path, block_size=4096)
        assert struct.unpack_from("<i", Path(path).read_bytes(), 8)[0] == 4096
        assert data.set_experiment_info("snapshot test")
        channel = data.create_channel("EEG", data_rate=400.0)
        assert channel is not None
        data.flush()
        primary = contents(data._pvfs, data.EXPERIMENT_DB_FILENAME)
        assert primary == contents(data._pvfs, data.EXPERIMENT_DB_BACKUP_FILENAME)
        snapshot = tmp_path / "verify.db3"
        snapshot.write_bytes(primary)
        with sqlite3.connect(snapshot) as db:
            assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        before = Path(path).read_bytes()
        reader = PvfsDataFile(read_only=True)
        try:
            assert reader.open(path)
            assert reader.get_experiment_info().name == "snapshot test"
            reader.flush()
            assert reader.close()
        finally:
            reader.close()
        assert Path(path).read_bytes() == before
        assert data.close()
        # Keep data and channel alive: close must still release all native handles.
        reopened = PvfsFile.open(path)
        reopened.close()
    finally:
        data.close()


@pytest.mark.parametrize("failure", ["replace", "commit", "backup"])
def test_database_failure_preserves_backup_and_releases_lock(tmp_path, monkeypatch, failure):
    path = str(tmp_path / "experiment.pvfs")
    data = PvfsDataFile()
    assert data.create(path)
    old_primary = contents(data._pvfs, data.EXPERIMENT_DB_FILENAME)
    old_backup = contents(data._pvfs, data.EXPERIMENT_DB_BACKUP_FILENAME)
    assert data.set_experiment_info("updated")
    calls = []
    original_overwrite = data._pvfs.overwrite
    original_flush = binding.PvfsFileHandle.flush

    def overwrite(name, snapshot, **kwargs):
        calls.append(name)
        assert Path(snapshot).exists()
        if failure == "replace" or (failure == "backup" and name == data.EXPERIMENT_DB_BACKUP_FILENAME):
            raise RuntimeError("injected replacement failure")
        return original_overwrite(name, snapshot, **kwargs)

    def flush(handle, commit=False):
        if failure == "commit" and commit:
            raise RuntimeError("injected commit failure")
        return original_flush(handle, commit)

    try:
        with monkeypatch.context() as patch:
            patch.setattr(data._pvfs, "overwrite", overwrite)
            patch.setattr(binding.PvfsFileHandle, "flush", flush)
            assert not data._save_database()
        assert calls == ([data.EXPERIMENT_DB_FILENAME, data.EXPERIMENT_DB_BACKUP_FILENAME]
                         if failure == "backup" else [data.EXPERIMENT_DB_FILENAME])
        assert contents(data._pvfs, data.EXPERIMENT_DB_BACKUP_FILENAME) == old_backup
        if failure == "replace":
            assert contents(data._pvfs, data.EXPERIMENT_DB_FILENAME) == old_primary
        assert not list(tmp_path.glob("temp_snapshot_*.db3"))
        assert data._save_database()
    finally:
        data.close()


def test_database_commits_once_per_entry(tmp_path, monkeypatch):
    data = PvfsDataFile()
    assert data.create(str(tmp_path / "experiment.pvfs"))
    flags = []
    original = binding.PvfsFileHandle.flush

    def flush(handle, commit=False):
        flags.append(commit)
        return original(handle, commit)

    try:
        monkeypatch.setattr(binding.PvfsFileHandle, "flush", flush)
        assert data._save_database()
        assert flags == [True, True]
        flags.clear()
        assert data._save_database(commit=False)
        assert flags == []
    finally:
        data.close()


def test_failed_close_retains_working_database_for_retry(tmp_path, monkeypatch):
    data = PvfsDataFile()
    assert data.create(str(tmp_path / "experiment.pvfs"))
    working_path = Path(data._temp_db_path)
    try:
        with monkeypatch.context() as patch:
            patch.setattr(data, "_save_database", lambda **kwargs: False)
            with pytest.raises(RuntimeError, match="save"):
                data.flush()
            assert not data.close()
            assert working_path.exists()
            assert data.is_open
            current_vfs = data._pvfs
            assert not data.create(str(tmp_path / "another.pvfs"))
            assert not data.open(str(tmp_path / "another.pvfs"))
            assert data._pvfs is current_vfs
            assert working_path.exists()
            assert not (tmp_path / "another.pvfs").exists()
        assert data.close()
        assert not working_path.exists()
    finally:
        data.close()
