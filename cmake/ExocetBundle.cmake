# What Exocet's kits run with, shipped beside the executable:
#
#   python/         a standalone CPython (python-build-standalone), with the lz4 package in its own
#                   site-packages at the version the crass build pins
#   exocet-tools/   salvador, passed to every kit command as EXOCET_TOOLS
#
# Not ports, and not "compatible" versions: a disk Peekaboo writes through a production's kit must
# be the disk that production's build writes, byte for byte, so these are the build's own pins -
# Python 3.12.10, lz4 4.4.5, salvador 1.4.1 (the crass repo's tools/requirements.txt). Change them
# together with it. Each download is checked against its published SHA-256.
#
# Off by default - it downloads about 30 MB - and on in the release presets. Without it the kit
# runs with the Python and tools directory the preferences name.

option(ADM_EXOCET_BUNDLE "Ship a standalone Python with the pinned lz4, and salvador, for Exocet's kits" OFF)
if(NOT ADM_EXOCET_BUNDLE)
    return()
endif()

set(EXOCET_PYTHON_RELEASE 20250409)
set(EXOCET_PYTHON_VERSION 3.12.10)
set(EXOCET_LZ4_VERSION 4.4.5)
set(EXOCET_SALVADOR_COMMIT d7ac9966d4b4015c9efd2166fd3bd55473bececd) # tag 1.4.1

if(WIN32)
    set(_py_triple x86_64-pc-windows-msvc)
    set(_py_sha 5ac66ae49a2104efeba985c1dc1cb40987757ee81cc6c7f218d10b801f3276ed)
    set(_py_exe python/python.exe)
elseif(APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_py_triple aarch64-apple-darwin)
        set(_py_sha 2d6477ecd10191675b7e7979e4b9e811fef36833ef3a7f3aa445eec305ce59a2)
    else()
        set(_py_triple x86_64-apple-darwin)
        set(_py_sha d0d51fa22c0e99b58de1b1b4baeca467d6fd0a1424c7509ea280c0796306c481)
    endif()
    set(_py_exe python/bin/python3)
else()
    set(_py_triple x86_64-unknown-linux-gnu)
    set(_py_sha e9cf6f7da499a4400ba30ae1da8f7ef25ce97827bd8c1084717aa05438035186)
    set(_py_exe python/bin/python3)
endif()

set(_exo_dir ${CMAKE_BINARY_DIR}/_deps/exocet)
set(_py_name cpython-${EXOCET_PYTHON_VERSION}+${EXOCET_PYTHON_RELEASE}-${_py_triple}-install_only.tar.gz)
set(_py_tar ${_exo_dir}/${_py_name})
set(_py_stamp ${CMAKE_BINARY_DIR}/python/.exocet-${EXOCET_PYTHON_VERSION}-lz4-${EXOCET_LZ4_VERSION})

# The Python, once per build directory: downloaded, checked, unpacked beside the executable, and
# the lz4 package installed into it. The stamp names both versions, so a bump redoes it.
if(NOT EXISTS ${_py_stamp})
    message(STATUS "Exocet: fetching Python ${EXOCET_PYTHON_VERSION} (${_py_triple})")
    file(DOWNLOAD
        https://github.com/astral-sh/python-build-standalone/releases/download/${EXOCET_PYTHON_RELEASE}/${_py_name}
        ${_py_tar} EXPECTED_HASH SHA256=${_py_sha} STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        message(FATAL_ERROR "Exocet: could not download ${_py_name}: ${_st}")
    endif()
    file(REMOVE_RECURSE ${CMAKE_BINARY_DIR}/python)
    file(ARCHIVE_EXTRACT INPUT ${_py_tar} DESTINATION ${CMAKE_BINARY_DIR})
    execute_process(
        COMMAND ${CMAKE_BINARY_DIR}/${_py_exe} -m pip install --disable-pip-version-check
                --no-warn-script-location --only-binary=:all: lz4==${EXOCET_LZ4_VERSION}
        RESULT_VARIABLE _pip OUTPUT_VARIABLE _pip_out ERROR_VARIABLE _pip_out)
    if(NOT _pip EQUAL 0)
        message(FATAL_ERROR "Exocet: could not install lz4 ${EXOCET_LZ4_VERSION} into the bundled "
                            "Python:\n${_pip_out}")
    endif()
    file(WRITE ${_py_stamp} "${_py_name}\nlz4 ${EXOCET_LZ4_VERSION}\n")
endif()

# salvador. On Windows the release's own binary - the one the crass repo carries, checked by its
# hash; elsewhere the release's source, built here.
set(_tools ${CMAKE_BINARY_DIR}/exocet-tools)
if(WIN32)
    if(NOT EXISTS ${_tools}/salvador.exe)
        set(_zip ${_exo_dir}/salvador_win64_1.4.1.zip)
        file(DOWNLOAD
            https://github.com/emmanuel-marty/salvador/releases/download/1.4.1/salvador_win64_1.4.1.zip
            ${_zip} EXPECTED_HASH SHA256=c3afe6b6ff6d8238745b65f8e6b8494eb99f767e06f2c315d88638d78abf7518
            STATUS _st)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "Exocet: could not download salvador: ${_st}")
        endif()
        file(ARCHIVE_EXTRACT INPUT ${_zip} DESTINATION ${_tools})
        file(SHA256 ${_tools}/salvador.exe _salv_sha)
        if(NOT _salv_sha STREQUAL "3441725af9519cb82c97e4448f3cb4f12f7ebe2e5d997827fc31ba2c0a37085c")
            file(REMOVE ${_tools}/salvador.exe)
            message(FATAL_ERROR "Exocet: salvador.exe is not the 1.4.1 release's binary")
        endif()
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(salvador
        GIT_REPOSITORY https://github.com/emmanuel-marty/salvador.git
        GIT_TAG ${EXOCET_SALVADOR_COMMIT})
    FetchContent_GetProperties(salvador)
    if(NOT salvador_POPULATED)
        FetchContent_Populate(salvador)
    endif()
    add_executable(salvador
        ${salvador_SOURCE_DIR}/src/salvador.c
        ${salvador_SOURCE_DIR}/src/expand.c
        ${salvador_SOURCE_DIR}/src/matchfinder.c
        ${salvador_SOURCE_DIR}/src/shrink.c
        ${salvador_SOURCE_DIR}/src/libdivsufsort/lib/divsufsort.c
        ${salvador_SOURCE_DIR}/src/libdivsufsort/lib/divsufsort_utils.c
        ${salvador_SOURCE_DIR}/src/libdivsufsort/lib/sssort.c
        ${salvador_SOURCE_DIR}/src/libdivsufsort/lib/trsort.c)
    target_include_directories(salvador PRIVATE ${salvador_SOURCE_DIR}/src
                                                ${salvador_SOURCE_DIR}/src/libdivsufsort/include)
    set_target_properties(salvador PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${_tools})
endif()

message(STATUS "Exocet: bundling ${_py_exe} with lz4 ${EXOCET_LZ4_VERSION}, and salvador, in ${CMAKE_BINARY_DIR}")
