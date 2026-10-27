# pypvfs

**`pypvfs`** is the PyPI package name for **Pinnacle Technology PVFS** — a virtual filesystem format used for physiological recordings. It is **not** the historical HPC “Parallel Virtual File System” or other unrelated uses of the “PVFS” acronym.

Install from PyPI, then import the **`pvfs_tools`** package:

```bash
pip install pypvfs
```

```python
from pvfs_tools.Core.pvfs_data_file import PvfsDataFile
```

**`pvfs_tools`** provides:

- **`pvfs_tools.Core`** — low-level PVFS access via `ctypes` and bundled native libraries (`pvfs.dll` / `pvfs_wrapper.dll` on Windows, `libpvfs.so` / `libpvfs_wrapper.so` on Linux).
- **`pvfs_tools.Database`** — higher-level helpers for experiment databases (SQLAlchemy-backed).

Runtime dependencies are listed in `pyproject.toml` (`numpy`, `pydantic`, `SQLAlchemy`, `Pillow`).

## Supported platforms (PyPI wheels)

Published wheels target **64-bit x86** only:

| Platform   | Wheel tag (typical)              |
|------------|----------------------------------|
| Linux      | `manylinux_2_28_x86_64`         |
| Windows    | `win_amd64`                      |

Wheels are tagged **`py3-none-…`**: one wheel per OS covers **Python 3.10+** on that OS (the native code does not link against `libpython`).

There is **no** published wheel today for **macOS**, **Linux ARM**, **musl** (Alpine), or **32-bit** Windows. On those environments, install from the **sdist** or a git checkout so the native libraries are built locally (see below).

## Optional extras

| Extra        | Installs | Use case |
|--------------|----------|----------|
| `video`      | `av` (PyAV) | WebM / VP8 export (`WebMWriter` and the video example GUI). |
| `examples` | `pyedflib`, `av` | Tk GUIs that need both EDF export and video/WebM paths. |
| `test`     | `pytest`, `av`   | Run the project test suite locally. |

Examples:

```bash
pip install "pypvfs[video]"
pip install "pypvfs[examples]"
pip install -e ".[test]"   # from a clone: editable install + tests
```

## Native library and building from source

**From a wheel:** native libraries are already built and installed under `pvfs_tools.Core`. No compiler is required.

**From source** (sdist or git clone): the build uses [**scikit-build-core**](https://scikit-build-core.readthedocs.io/) and **CMake** to compile `pvfs.cpp` and `pvfs_wrapper.cpp`. You need:

- CMake ≥ 3.15 (often pulled into the build environment automatically)
- A **C++17** toolchain (`g++` / `clang++` on Linux, MSVC on Windows)

```bash
pip install ".[test]"
pytest
```

**Editable installs:** `python -m pip install -e .` builds and installs the native
libraries while loading Python code from `src/pvfs_tools/`. The binding searches
the installed package path for native libraries before checking the source tree.
Python edits apply immediately; after C++ edits, rerun the install command and
restart Python to load the rebuilt libraries.

On Windows, the default Visual Studio build needs the **Desktop development with
C++** workload and a complete Windows SDK. If CMake reports no C++ compiler even
though Visual Studio is installed, check `CMakeFiles/CMakeConfigureLog.yaml` in
the build directory. Missing `ucrtd.lib` or `ucrt/stdio.h` indicates incomplete
SDK components; repair/reinstall the SDK through Visual Studio Installer.

Alternatively, select an installed LLVM-MinGW compiler explicitly with Ninja.
Use a separate build directory when switching away from Visual Studio, and
replace the compiler path below with your installation:

```powershell
$env:CMAKE_GENERATOR = "Ninja"
python -m pip install -e . `
  "--config-settings=build-dir=build/editable-clang" `
  "--config-settings=cmake.define.CMAKE_CXX_COMPILER=C:/path/to/llvm-mingw/bin/clang++.exe"
```

Ninja must be available on `PATH`. This environment setting affects the current
PowerShell session; remove it with `Remove-Item Env:CMAKE_GENERATOR` before
switching back to Visual Studio.

After updating the native sources or bindings, rebuild **both** native libraries
with the same compiler and runtime. The read-only and overwrite bindings require
the updated wrapper; previously built DLLs or shared libraries are not sufficient.
MinGW installs include matching runtime DLLs from the compiler's directory.

Native wrapper regression checks can be run alongside `pytest`:

```bash
cmake -S . -B build/native-tests -DPVFS_BUILD_TESTS=ON
cmake --build build/native-tests --config Release
ctest --test-dir build/native-tests -C Release --output-on-failure
```

## Examples (bundled in the wheel)

Runnable scripts are installed under **`pvfs_tools.examples`**. After `pip install pypvfs` you can use the **`pypvfs-examples`** command on your `PATH`, or run modules with `python -m`:

| Module | Suggested install | Description |
|--------|-------------------|-------------|
| `pvfs_tools.examples.pvfs_create_cli` | base package | CLI: synthetic PVFS file |
| `pvfs_tools.examples.pvfs_to_edf_converter` | `pypvfs[examples]` (needs `pyedflib`) | Tk: PVFS → EDF+ |
| `pvfs_tools.examples.pvfs_to_video_converter` | `pypvfs[video]` or `pypvfs[examples]` (needs `av`) | Tk: PVFS video → WebM |

```bash
pypvfs-examples --list
python -m pvfs_tools.examples.pvfs_create_cli --help
```

The same `.py` files also live under [`examples/`](examples/) in the repository for browsing in GitHub.

## Documentation and source

- **PyPI** shows this README; the **BSD 3-Clause** license text is included in package metadata (see the `LICENSE` file in the installed `.dist-info` / sdist).
- **Library usage (detailed):** [docs/USAGE.md](docs/USAGE.md) — workflows, APIs, and patterns; **not** in the wheel; browse on GitHub or in a clone. Written to match [`tests/test_pvfs.py`](tests/test_pvfs.py).
- **Source and issues:** [github.com/Pinnacle-Technology-Inc/pypvfs](https://github.com/Pinnacle-Technology-Inc/pypvfs) (also set as `Repository` / `Documentation` in `pyproject.toml` for `pip show` / PyPI links).
- **Releases (maintainers):** see [docs/RELEASE.md](docs/RELEASE.md) in the repo — not installed into `site-packages`.

## Publishing (maintainers)

Routine releases use GitHub Actions and [PyPI trusted publishing](https://docs.pypi.org/trusted-publishers/) (OIDC), not ad-hoc `twine` from a laptop.

1. Bump `version` in `pyproject.toml` and commit.
2. Create and push a tag whose name starts with **`v`** (e.g. `git tag v1.0.6 && git push origin v1.0.6`).
3. Confirm the **Publish to PyPI** workflow ran for that tag (or run it manually from the Actions tab).

That workflow uses [**cibuildwheel**](https://cibuildwheel.pypa.io/) on Linux and Windows to build the platform wheels and uploads wheels plus the sdist. Details: [docs/RELEASE.md](docs/RELEASE.md).
