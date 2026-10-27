# Using `pypvfs` (`pvfs_tools`)

This guide describes how to use the **pypvfs** distribution’s Python package **`pvfs_tools`**. It is aligned with the behaviour covered by the unit tests in [`tests/test_pvfs.py`](../tests/test_pvfs.py). For exact expectations (fixtures, assertions, cleanup), treat that file as the source of truth.

**This file lives only in the Git repository**; it is not shipped inside the PyPI wheel. After `pip install pypvfs`, open this document on [GitHub](https://github.com/Pinnacle-Technology-Inc/pypvfs/blob/main/docs/USAGE.md) or clone the repo.

**Note:** pvfs is a generically useful virtual file system.  This layer is available through pvfs_tools/Core/pvfs.cpp, the compiled libraries and python wrappers.  Separate from this is the explicit form used for Pinnacle Technology data acquisition.  This implements a structured data and metadata storage approach within the virtual files system.  It is available via indexed_data_file.py, pvfs_data_file.py, video_data_file.py and the database handlers found in pvfs_tools/Database.  Install  with pip install pypvfs[examples] and consult the examples under pvfs_tools/examples as a quick start.

**Note:** a companion Python project that uses this library to stream and store data from Pinnacle devices can be found at https://github.com/Pinnacle-Technology-Inc/Morelia.
---

## Install and optional pieces

```bash
pip install pypvfs
```

| Goal | Command |
|------|---------|
| WebM / VP8 export (`WebMWriter`, video example) | `pip install "pypvfs[video]"` |
| Tk examples that need **both** EDF + video stacks | `pip install "pypvfs[examples]"` |
| Run the **test suite** (same file as below) | `pip install "pypvfs[test]"` |

The tests import **PyAV** (`av`) at module level and skip entirely if it is missing—install **`[test]`** (or **`[video]`** / **`[examples]`**) before running `pytest`.

---

## Package layout

| Area | Role |
|------|------|
| **`pvfs_tools.Core.pvfs_binding`** | Low-level **`PvfsFile`**, **`PvfsFileHandle`**, **`HighTime`**, **`StringVector`** — thin `ctypes` layer over the native PVFS libraries. |
| **`pvfs_tools.Core.pvfs_data_file`** | **`PvfsDataFile`** — higher-level create/open/close, experiment DB integration, **`IndexedDataFile`** channels. |
| **`pvfs_tools.Core.indexed_data_file`** | **`IndexedDataFile`** — read/write time-series blocks inside a PVFS (`.index` / `.idat` pairs). |
| **`pvfs_tools.Core.video_data_file`** | **`VideoDataFile`** — VP8-style video payloads inside a PVFS (used with optional PyAV for export). |
| **`pvfs_tools.Core.webm_helpers`** | **`WebMWriter`** — mux WebM (requires **`[video]`**). |
| **`pvfs_tools.Database.database`** | **`ExperimentDatabase`** — SQLite experiment metadata (SQLAlchemy); lines up with `experiment.db3` inside a PVFS. |
| **`pvfs_tools.Database.models`** | **`ExperimentInformation`**, **`ChannelInformation`**, **`Annotation`**, etc. |
| **`pvfs_tools.Database.exceptions`** | **`TableError`**, **`DatabaseError`**, … |

Runnable examples ship in **`pvfs_tools.examples`** after install; see the top-level **README** for `pypvfs-examples` and `python -m pvfs_tools.examples.…`.

---

## PVFS concepts (minimal)

A **`.pvfs` file** is a container of **named files** (channels). Typical names you will see (see `test_pvfs_get_channel_list`):

- `experiment.db3`, `experiment_backup.db3`
- Indexed EEG-style channels: base name + **`.index`** / **`.idat`** (e.g. `EEG10.index`, `EEG10.idat`)

**`PvfsFile.get_channel_list()`** and **`get_file_list()`** return string collections (iterable; tests use `len(...)`, `set(...)`, and `in` checks).

---

## `PvfsFile` — open, create, lock, read/write, extract

### Open an existing PVFS

```python
from pvfs_tools.Core.pvfs_binding import PvfsFile

vfs = PvfsFile.open("/path/to/recording.pvfs", read_only=True)
try:
    names = list(vfs.get_channel_list())
    files = list(vfs.get_file_list())
finally:
    vfs.close()
```

Always **`close()`** when finished (tests also use `gc.collect()` and short sleeps on Windows so the OS releases file handles before deleting temp `.pvfs` files).

The default `read_only=False` requests a writable handle and fails if another
writer holds the recording. Use `read_only=True` for analysis or extraction,
including while Sirenia is recording. A writable open never falls back to a
reader. Readers see flushed data; this is not a transactionally consistent
snapshot of an actively changing recording.

### Create a new empty PVFS

```python
vfs = PvfsFile.create("/path/to/new.pvfs")  # optional block_size=... (see docstring in code)
assert vfs.is_open
vfs.close()
```

### VFS-wide lock

Many multi-step operations expect the VFS to be **locked** during writes (see `blank_pvfs_with_experiment_db`, `test_pvfs_single_file_write_extract`, `test_pvfs_database_extract_and_write`):

```python
vfs.lock()
try:
    # ... open handles, write, flush, close handles ...
    pass
finally:
    vfs.unlock()
```

### Read a member file by name

```python
vfs.lock()
try:
    h = vfs.open_file("experiment.db3")
    info = h.get_file_info()
    size = int(info.size)
    raw = h.read(size) if size > 0 else b""
    h.close()
finally:
    vfs.unlock()
```

`get_file_info()` exposes **`startBlock`**, **`size`**, and a fixed **`filename`** buffer (tests decode with `bytes(info.filename).decode("utf-8", errors="ignore").rstrip("\x00")`).

### Create a new member file and write (`fcreate` / `write` / `flush` / `close`)

Pattern from **`test_pvfs_single_file_write_extract`** and **`test_pvfs_database_extract_and_write`**:

```python
data: bytes = b"..."
CHUNK = 1024  # tests match 1 KiB chunks when mirroring PVFS_add behaviour

vfs.lock()
try:
    dst = vfs.fcreate("experiment.db3")
    offset = 0
    while offset < len(data):
        chunk = data[offset : offset + CHUNK]
        n = dst.write(chunk, len(chunk))
        assert n == len(chunk)
        dst.flush()
        offset += n
    dst.close()
finally:
    vfs.unlock()
```

`PvfsFileHandle.write` takes **`(buffer, length)`**; buffers are typically **`bytes`** (tests also use `ctypes` buffers for bulk copies in fixtures).

### Extract a member file to disk

```python
rc = vfs.extract("experiment.db3", "/tmp/out.db3")
assert rc == 0
```

On failure the binding may raise **`RuntimeError`** instead of returning a negative code—see **`PvfsFile.extract`** in `pvfs_binding.py`.

`extract()` takes the PVFS lock internally and checks both reads and writes.
Call it outside an existing `vfs.lock()` section, since the native lock is not
recursive. A failed extraction may leave a partial output file.

### Replace a member from a disk file

```python
vfs.overwrite("experiment.db3", "/tmp/snapshot.db3")
```

`overwrite()` stages the replacement before updating the directory entry and
creates the member if it does not exist. Close any handles to that member first.
It locks internally; use `lock=False` only when already holding the VFS lock.
It raises `RuntimeError` on failure. The final directory update is not atomic
against power loss. To force OS buffers to storage, open the resulting member
and call `handle.flush(commit=True)`; ordinary `flush()` only writes buffers.

The low-level `PvfsIndexHeaderWrapper.startTime` and `.endTime` fields are nested
`PvfsHighTimeWrapper` values with `.seconds` and `.subseconds`, matching the
native 56-byte wrapper. The wrapper uses a ten-second timestamp interval when
writing headers; use `IndexedDataFile` for configurable timestamp intervals.

---

## `HighTime` — timestamps inside PVFS

Used everywhere indexed data and the DB care about sub-second time.

```python
from pvfs_tools.Core.pvfs_binding import HighTime

ht = HighTime(1609459200, 0.5)           # int seconds + float subseconds
assert ht.seconds == 1609459200
assert ht.subseconds == 0.5

ht2 = HighTime.from_seconds(time.time())  # float wall time → HighTime
span = ht2.to_seconds()
```

Tests use **`+`** on **`HighTime`** and floats for windows (e.g. `segment_stop = start_time + 2` in **`test_indexed_data_file`**).

---

## `IndexedDataFile` — read (and append when creating)

### Open on an existing **`PvfsFile`** by **base name**

The constructor takes the **stem** used in the PVFS (without forcing you to pass `.index`). Tests use the **`EEG10`** channel like this:

```python
from pvfs_tools.Core.indexed_data_file import IndexedDataFile

idx = IndexedDataFile(vfs, "EEG10")
try:
    header = idx._header
    assert header.data_rate > 0

    t0 = idx.get_start_time()
    t1 = idx.get_end_time()
    assert t0.to_seconds() < t1.to_seconds()
    assert idx.get_channel_name() == "EEG10"

    timestamps, values = idx.get_data(t0, t0 + 2.0)  # HighTime window
    assert len(timestamps) == len(values)
finally:
    idx.close()
```

### Creating channels via **`PvfsDataFile`** (recommended)

When **authoring** PVFS content, tests use **`PvfsDataFile.create`**, **`create_channel`**, **`append_block`**, and **`close()`** so `experiment.db3` and index/idat pairs stay consistent (**`test_pvfs_create_file`**).

Important detail from that test: the **on-disk base name** is **`channel_name + str(channel_id)`** (first channel `EEG0` → files `EEG00.*`; second `EEG1` → `EEG11.*` because `id == 1`). Pass that stem to **`IndexedDataFile(vfs, "EEG00", channel_name="EEG0")`** when opening raw **`PvfsFile`** instances.

---

## `PvfsDataFile` — high-level create / channels / flush

```python
from pvfs_tools.Core.pvfs_data_file import PvfsDataFile
from pvfs_tools.Core.pvfs_binding import HighTime
import math, time

pvfs_path = "/tmp/demo.pvfs"
pdf = PvfsDataFile()
assert pdf.create(pvfs_path)

sample_rate = 400.0
start_time = HighTime.from_seconds(time.time())
idf = pdf.create_channel("EEG0", data_rate=sample_rate, unit="uV")
idf._delta_time = HighTime(0, 1.0 / sample_rate)

t = [i / sample_rate for i in range(1000)]
values = [50.0 * math.sin(2 * math.pi * 10 * ti) for ti in t]
assert idf.append_block(start_time, values) == 0

pdf.close()
```

`create()` builds a temporary SQLite DB next to the PVFS, writes **`experiment.db3`** / backup into the container, and keeps schema aligned with real recordings. Always call **`close()`** so buffers flush into the PVFS (tests wait, then reopen with **`PvfsFile.open`** to verify).

Use `PvfsDataFile(path, read_only=True)` for reading. Its flush and close paths
do not save metadata back to the recording. Writable database saves replace
primary and backup from the same staged snapshot under the PVFS lock, stopping
before the backup if the primary replacement or commit fails. Synchronous saves
commit once per completed entry. `flush()` raises on save failure; `close()`
returns `False` and retains the working database for a retry.

---

## `ExperimentDatabase` — metadata after extract (or standalone file)

Typical test pattern: **extract** `experiment.db3`, then open the file path:

```python
from pvfs_tools.Database.database import ExperimentDatabase
from pvfs_tools.Database.exceptions import TableError

db = ExperimentDatabase("/tmp/extracted.db3")
try:
    info = db.get_information()
    names = db.get_channel_names()
    ch = db.get_channel_info("CH C")          # may skip if name missing
    annos = db.get_channel_annotations(ch.id)
    all_ann = db.get_all_annotations()
finally:
    db.close()
```

To **bootstrap an empty** DB file on disk (before embedding it into a PVFS), tests construct **`ExperimentDatabase(filename=..., in_memory=False)`**, call **`create(path)`**, **`close()`**, then read raw bytes and **`fcreate`/`write`** in chunks—see **`blank_pvfs_with_experiment_db`** and **`test_pvfs_create_database`**.

---

## Video / WebM (optional **`av`**)

- **`VideoDataFile`** — read VP8-style tracks stored in PVFS (see `video_data_file.py`; tests focus on indexed EEG + DB).
- **`WebMWriter`** — requires **`pip install "pypvfs[video]"`**; see **`webm_helpers.py`** and **`pvfs_tools.examples.pvfs_to_video_converter`**.

---

## Examples and discovery

After install:

```bash
pypvfs-examples --list
python -m pvfs_tools.examples.pvfs_create_cli --help
```

The same scripts exist under **`examples/`** in the repository.

---

## Running the unit tests (ground truth)

From a clone of the repository:

```bash
pip install -e ".[test]"
pytest -q tests/test_pvfs.py
```

The suite uses bundled fixtures under **`tests/`** (for example **`sine.pvfs`**, **`test.db3`**). It documents:

- channel / file listing,
- extract + SQLite verification,
- **`fcreate` / write / extract** round-trips,
- full **`PvfsDataFile`** synthetic build with two indexed channels,
- **`ExperimentDatabase`** queries,
- **`IndexedDataFile.get_data`** windows,
- **`HighTime`** and **`PvfsFile` lock/unlock** behaviour.

If behaviour and this guide disagree, **trust the tests** (or update the tests and then this file).

---

## Related repository docs

- **[RELEASE.md](RELEASE.md)** — tagging and PyPI publish workflow for maintainers.
