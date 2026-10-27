cmake_minimum_required(VERSION 3.15)
project(pypvfs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# All native sources live alongside the Python package they back.
set(PVFS_CORE_DIR "${CMAKE_SOURCE_DIR}/src/pvfs_tools/Core")

# ---------- libpvfs (the core C++ library) ----------------------------------

add_library(pvfs SHARED
    "${PVFS_CORE_DIR}/pvfs.cpp"
)
target_include_directories(pvfs PRIVATE "${PVFS_CORE_DIR}")
set_target_properties(pvfs PROPERTIES
    OUTPUT_NAME "pvfs"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN"
)

# Export the core symbols with both MSVC and MinGW on Windows.
target_compile_definitions(pvfs PRIVATE PVFS_LIBRARY)

# ---------- libpvfs_wrapper (the C ABI surface ctypes loads) ----------------

add_library(pvfs_wrapper SHARED
    "${PVFS_CORE_DIR}/pvfs_wrapper.cpp"
)
target_include_directories(pvfs_wrapper PRIVATE "${PVFS_CORE_DIR}")
target_link_libraries(pvfs_wrapper PRIVATE pvfs)
set_target_properties(pvfs_wrapper PROPERTIES
    OUTPUT_NAME "pvfs_wrapper"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN"
)

# ---------- Platform-specific compile/link tuning ---------------------------

if(WIN32)
    # MinGW defaults to lib*.dll; ctypes uses the same filenames as MSVC.
    set_target_properties(pvfs pvfs_wrapper PROPERTIES PREFIX "")
endif()

if(MSVC)
    # /wd4251: STL types crossing DLL boundary (intentional in this wrapper).
    target_compile_options(pvfs         PRIVATE /wd4251 /O2)
    target_compile_options(pvfs_wrapper PRIVATE /wd4251 /O2)
    # Match upstream PVFS DLL runtime choice (MD / MDd).
    set_property(TARGET pvfs         PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_property(TARGET pvfs_wrapper PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_definitions(pvfs         PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(pvfs_wrapper PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX AND NOT APPLE)
    # Statically embed libstdc++ / libgcc so the wheel does not require a
    # specific GLIBCXX / CXXABI version on the user's system. Without this,
    # the .so files end up depending on whichever libstdc++ the build host
    # shipped (e.g. GLIBCXX_3.4.32 from GCC 13 on Ubuntu 24.04), which is
    # what silently broke pypvfs 1.0.1 on every pre-Ubuntu-24.04 Linux.
    target_link_options(pvfs         PRIVATE -static-libstdc++ -static-libgcc)
    target_link_options(pvfs_wrapper PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---------- Install rules (scikit-build-core picks these up) ----------------

option(PVFS_BUILD_TESTS "Build native wrapper contract tests" OFF)
if(PVFS_BUILD_TESTS)
    enable_testing()
    add_executable(pvfs_wrapper_contract tests/native_wrapper_contract.cpp)
    target_include_directories(pvfs_wrapper_contract PRIVATE "${PVFS_CORE_DIR}")
    target_link_libraries(pvfs_wrapper_contract PRIVATE pvfs)
    add_test(NAME pvfs_wrapper_contract COMMAND pvfs_wrapper_contract)
endif()

# Both libraries share an installed package directory. The binding locates it
# through the package search path, including when Python sources are editable.
install(TARGETS pvfs pvfs_wrapper
    LIBRARY DESTINATION pvfs_tools/Core   # .so on Linux / .dylib on macOS
    RUNTIME DESTINATION pvfs_tools/Core   # .dll on Windows
)

if(MINGW)
    # Bundle matching compiler runtimes for wheels and editable installs.
    get_filename_component(PVFS_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime IN ITEMS libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll
                             libc++.dll libunwind.dll)
        if(EXISTS "${PVFS_COMPILER_BIN}/${runtime}")
            install(FILES "${PVFS_COMPILER_BIN}/${runtime}" DESTINATION pvfs_tools/Core)
        endif()
    endforeach()
endif()

# Ship the top-level examples/ directory inside the installed package so users
# can run them with:
#     python -m pvfs_tools.examples.pvfs_create_cli ...
#
# A generated __init__.py makes pvfs_tools.examples a real sub-package without
# requiring one in the GitHub-visible source tree.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/examples/"
    DESTINATION pvfs_tools/examples
    FILES_MATCHING
        PATTERN "*.py"
)

install(CODE [[
    file(WRITE "${CMAKE_INSTALL_PREFIX}/pvfs_tools/examples/__init__.py"
"\"\"\"Runnable examples shipped with pypvfs.

Each module here is a standalone script with an ``if __name__ == \"__main__\"``
block. Run any example directly with::

    python -m pvfs_tools.examples.pvfs_create_cli --help
    python -m pvfs_tools.examples.pvfs_to_edf_converter
    python -m pvfs_tools.examples.pvfs_to_video_converter

Note: pvfs_to_edf_converter requires the [examples] extra (pyedflib), and
pvfs_to_video_converter requires the [video] extra (av).
\"\"\"
")
]])
