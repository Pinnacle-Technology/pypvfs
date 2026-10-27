"""Editable installs must load rebuilt libraries from the installed package path."""

import sys

from pvfs_tools.Core import pvfs_binding as binding


def test_editable_library_precedes_source_library(tmp_path, monkeypatch):
    source = tmp_path / "source"
    installed = tmp_path / "installed"
    source.mkdir()
    installed.mkdir()
    name = "pvfs_wrapper.dll" if binding.os.name == "nt" else "libpvfs_wrapper.so"
    (source / name).touch()
    (installed / name).touch()
    monkeypatch.setattr(binding, "__file__", str(source / "pvfs_binding.py"))
    monkeypatch.setattr(sys.modules[binding.__package__], "__path__", [str(source), str(installed)])
    assert binding._library_path() == installed / name


def test_source_checkout_library(tmp_path, monkeypatch):
    name = "pvfs_wrapper.dll" if binding.os.name == "nt" else "libpvfs_wrapper.so"
    (tmp_path / name).touch()
    monkeypatch.setattr(binding, "__file__", str(tmp_path / "pvfs_binding.py"))
    monkeypatch.setattr(sys.modules[binding.__package__], "__path__", [str(tmp_path)])
    assert binding._library_path() == tmp_path / name
