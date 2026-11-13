cmake_minimum_required(VERSION 3.25)

# Keep vcpkg's port builds on the same compiler this project links with.
#
# The x64-mingw-* triplets declare `VCPKG_ENV_PASSTHROUGH PATH` and locate gcc/g++ through PATH,
# while CMake uses the compiler recorded in the cache. A second MinGW installation earlier in
# PATH therefore builds the dependencies with a *different* libstdc++ than we link against —
# which stays silent until the two libstdc++ versions disagree on an ABI detail. GCC 16 replaced
# libstdc++'s std::call_once thread-local variables (__once_call / __once_callable) with accessor
# functions, so ports built by GCC 16 fail to link against GCC 15 with:
#   undefined reference to `std::__get_once_call()' / `std::__get_once_callable()'
#
# Putting our compiler's directory first in the PATH that vcpkg inherits removes the ambiguity.
# CMAKE_CXX_COMPILER comes from the cache, so this applies from the first re-configure onwards
# (a fresh configure has no compiler yet and falls back to plain PATH order, as before).
if(WIN32 AND CMAKE_CXX_COMPILER)
    get_filename_component(_adm_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(IS_DIRECTORY "${_adm_compiler_dir}")
        file(TO_NATIVE_PATH "${_adm_compiler_dir}" _adm_compiler_dir_native)
        set(ENV{PATH} "${_adm_compiler_dir_native};$ENV{PATH}")
    endif()
    unset(_adm_compiler_dir)
    unset(_adm_compiler_dir_native)
endif()

# Auto-detect MinGW and set the correct vcpkg triplet before project() loads the toolchain.
# Without this, vcpkg defaults to x64-windows (MSVC) on Windows which is incompatible with MinGW.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(DEFINED ENV{MSYSTEM} OR CMAKE_C_COMPILER MATCHES "[Mm]in[Gg][Ww]")
        set(VCPKG_TARGET_TRIPLET "x64-mingw-static" CACHE STRING "")
        set(VCPKG_HOST_TRIPLET "x64-mingw-static" CACHE STRING "")
    elseif("$ENV{OS}" STREQUAL "Windows_NT" AND NOT MSVC)
        # Outside MSYS2 (e.g. IDE or cmd.exe): probe for MinGW gcc on PATH
        find_program(_GCC_PROBE gcc PATHS "C:/ProgramData/mingw64/mingw64/bin" ENV PATH)
        if(_GCC_PROBE AND NOT _GCC_PROBE MATCHES "[Cc]ygwin")
            set(VCPKG_TARGET_TRIPLET "x64-mingw-static" CACHE STRING "")
            set(VCPKG_HOST_TRIPLET "x64-mingw-static" CACHE STRING "")
        endif()
        unset(_GCC_PROBE CACHE)
    endif()
endif()

project(peekaboo VERSION 0.9.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies (via vcpkg) ---
find_package(imgui CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Crc32c CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
# zlib for the ZMBV capture codec. Already installed with libzip; declared so the capture code does
# not depend on libzip keeping it.
find_package(ZLIB REQUIRED)

# --- Local Dependencies ---
# adflib (managed by FetchContent)
include(FetchContent)

# Exocet's kits: a bundled Python with the pinned lz4, and salvador (release presets)
include(${CMAKE_SOURCE_DIR}/cmake/ExocetBundle.cmake)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(
  adflib
  GIT_REPOSITORY https://github.com/adflib/ADFlib.git
  GIT_TAG        master
)

set(ADFLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ADFLIB_ENABLE_ADDRESS_SANITIZER OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(adflib)

# xdms (for .dms support)
FetchContent_Declare(
  xdms
  GIT_REPOSITORY https://gitlab.com/heikkiorsila/xdms.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(xdms)

# cpp-httplib (header-only HTTP server for local Peekaboo API)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.1
)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# libdisk (from keirf/disk-utilities) — identifies named Amiga copy-protection
# track formats. We feed it the per-track MFM extracted via capsimg using its
# soft (in-memory) stream, then probe the format handlers. Optional: it is a
# large body of third-party C; disable if it fails to build on a platform.
option(ADM_ENABLE_LIBDISK "Build libdisk for IPF copy-protection identification" ON)
if(ADM_ENABLE_LIBDISK)
    FetchContent_Declare(
      disk_utilities
      GIT_REPOSITORY https://github.com/keirf/disk-utilities.git
      GIT_TAG        5e690f3aa248c4d66d2b233ba3e6f7147fbd0311
    )
    FetchContent_MakeAvailable(disk_utilities)

    # disk-utilities ships a Makefile, not CMake, so build the libdisk subset
    # here. Compile every format/container/stream unit (they cross-reference via
    # X-macro tables) but swap the proprietary CAPS stream for its stub: we do
    # our own capsimg decode and feed libdisk an in-memory soft stream.
    file(GLOB_RECURSE LIBDISK_FORMAT  "${disk_utilities_SOURCE_DIR}/libdisk/format/*.c")
    file(GLOB          LIBDISK_CONT   "${disk_utilities_SOURCE_DIR}/libdisk/container/*.c")
    file(GLOB          LIBDISK_STREAM "${disk_utilities_SOURCE_DIR}/libdisk/stream/*.c")
    list(FILTER LIBDISK_STREAM EXCLUDE REGEX "/caps\\.c$")

    add_library(libdisk STATIC
        ${disk_utilities_SOURCE_DIR}/libdisk/disk.c
        ${disk_utilities_SOURCE_DIR}/libdisk/util.c
        ${LIBDISK_FORMAT}
        ${LIBDISK_CONT}
        ${LIBDISK_STREAM}
    )
    target_include_directories(libdisk PUBLIC
        "${disk_utilities_SOURCE_DIR}/libdisk/include")
    # libdisk has its own crc32() (distinct from zlib's); rename it so it doesn't
    # clash at link time with the zlib pulled in by libzip. libdisk needs no zlib.
    target_compile_definitions(libdisk PRIVATE
        -D_GNU_SOURCE -D_CRT_SECURE_NO_WARNINGS -Dcrc32=adm_ld_crc32)
endif()

# xdms doesn't have a CMakeLists.txt so we define the target here manually
# We exclude xdms.c because it contains main()
file(GLOB XDMS_SOURCES "${xdms_SOURCE_DIR}/src/*.c")
list(FILTER XDMS_SOURCES EXCLUDE REGEX "xdms\\.c$")

add_library(xdms_lib STATIC ${XDMS_SOURCES})
target_include_directories(xdms_lib PUBLIC "${xdms_SOURCE_DIR}/src")
# xdms seems to need NO special flags, but let's be safe
target_compile_definitions(xdms_lib PRIVATE -D_CRT_SECURE_NO_WARNINGS)

# RECOIL - Retro Computer Image Library (single-file C library for Amiga image formats)
add_library(recoil STATIC external/recoil/recoil.c)
target_include_directories(recoil PUBLIC external/recoil)
target_compile_definitions(recoil PRIVATE -D_CRT_SECURE_NO_WARNINGS)

# stb_image - single header image decoder (PNG/JPG) + writer (PNG)
add_library(stb_image STATIC external/stb/stb_image_impl.cpp external/stb/stb_image_write_impl.cpp)
target_include_directories(stb_image PUBLIC external/stb)

# nanosvg - single header SVG parser and rasterizer
add_library(nanosvg STATIC external/nanosvg/nanosvg_impl.cpp)
target_include_directories(nanosvg PUBLIC external/nanosvg)

# miniaudio - single header audio library
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE external/miniaudio)

# libopenmpt - tracker music decoder
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENMPT REQUIRED IMPORTED_TARGET libopenmpt)

# Default to VS-style folders for source groups
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# --- Core Library ---
# Split core logic into a library so it can be linked by both the app and tests
add_library(adm_core STATIC
    src/core/AdfWrapper.cpp
    src/core/AdfWrapper.h
    src/core/BlockAnalyzer.cpp
    src/core/BlockAnalyzer.h
    src/core/BlockReader.cpp
    src/core/BlockReader.h
    src/core/FileOperations.cpp
    src/core/FileOperations.h
    src/core/ZipUtils.cpp
    src/core/ZipUtils.h
    src/core/SessionSerializer.cpp
    src/core/SessionSerializer.h
    src/core/AmigaEmulator.cpp
    src/core/KickstartRoms.cpp
    src/core/SystemFiles.cpp
    src/core/SystemFiles.h
    src/core/AviRecorder.cpp
    src/core/AviRecorder.h
    src/core/ZmbvEncoder.cpp
    src/core/ZmbvEncoder.h
    src/core/CaptureFrame.cpp
    src/core/CaptureFrame.h
    src/core/FfmpegRecorder.cpp
    src/core/FfmpegRecorder.h
    src/core/VideoCaptureSink.cpp
    src/core/VideoCaptureSink.h
    src/core/AmigaEmulator.h
    src/core/EmulatorOptions.cpp
    src/core/EmulatorOptions.h
    src/core/HostExecutableLauncher.cpp
    src/core/HostExecutableLauncher.h
    src/core/HostMountService.cpp
    src/core/HostMountService.h
    src/core/HardfileInspector.cpp
    src/core/HardfileInspector.h
    src/core/WhdloadLaunch.cpp
    src/core/CdMedia.cpp
    src/core/CdMedia.h
    src/core/MachineConfig.cpp
    src/core/MachineConfig.h
    src/core/CpuExtState.cpp
    src/core/CpuExtState.h
    src/core/HostPorts.cpp
    src/core/HostPorts.h
    src/core/WhdloadLaunch.h
    src/core/HostDirectoryDocument.cpp
    src/core/HostDirectoryDocument.h
    src/core/BitplaneDecoder.cpp
    src/core/BitplaneDecoder.h
    src/core/GifRecorder.cpp
    src/core/GifRecorder.h
    src/core/ClipboardImage.cpp
    src/core/ClipboardImage.h
    src/core/Disassembler.cpp
    src/core/Disassembler.h
    src/core/CrassProfile.cpp
    src/core/CrassProfile.h
    src/core/CrassMemMap.cpp
    src/core/CrassMemMap.h
    src/core/ExocetActivity.cpp
    src/core/ExocetActivity.h
    src/core/ExocetBlock.cpp
    src/core/ExocetBlock.h
    src/core/ExocetDiskMap.cpp
    src/core/ExocetDiskMap.h
    src/core/ExocetEval.cpp
    src/core/ExocetEval.h
    src/core/ExocetKit.cpp
    src/core/ExocetKit.h
    src/core/ExocetSession.cpp
    src/core/ExocetSession.h
    src/core/ExocetPack.cpp
    src/core/ExocetPack.h
    src/core/ExocetQuery.cpp
    src/core/ExocetQuery.h
    src/core/DebugSymbols.cpp
    src/core/DebugSymbols.h
    src/core/DebugLines.cpp
    src/core/DebugLines.h
    src/core/DebugInline.cpp
    src/core/DebugInline.h
    src/core/DebugFrame.cpp
    src/core/DebugFrame.h
    src/core/StackUnwinder.cpp
    src/core/StackUnwinder.h
    src/core/SourceDebugContext.cpp
    src/core/SourceDebugContext.h
    src/core/ElfHunkMap.cpp
    src/core/LineCoverage.cpp
    src/core/TimeTravel.cpp
    src/core/GdbPacketHandler.cpp
    src/core/SegListResolver.cpp
    src/core/SegListResolver.h
    src/core/FunctionProfiler.cpp
    src/core/FunctionProfiler.h
    src/core/CallStackProfiler.cpp
    src/core/CallStackProfiler.h
    src/core/AsmHighlighter.cpp
    src/core/AsmHighlighter.h
    src/core/CSourceHighlighter.cpp
    src/core/CSourceHighlighter.h
    src/core/EditorLauncher.cpp
    src/core/EditorLauncher.h
    src/core/SourceFileCache.cpp
    src/core/SourceFileCache.h
    src/core/MemoryAnnotations.cpp
    src/core/MemoryLocks.cpp
    src/core/TrainerFile.cpp
    src/core/AnchorResolver.cpp
    src/core/MemoryAnnotations.h
    src/core/LibraryCallAnnotator.cpp
    src/core/LibraryCallAnnotator.h
    src/core/ArConsoleTypes.h
    src/core/ArMemoryAccess.cpp
    src/core/ArMemoryAccess.h
    src/core/ArCommandExocet.cpp
    src/core/ArCommandInterpreter.cpp
    src/core/ArCommandInterpreter.h
    src/core/Assembler.h
    src/core/UaeAssembler.cpp
    src/core/UaeAssembler.h
    src/core/DmsConverter.cpp
    src/core/DmsConverter.h
    src/core/DmsParser.cpp
    src/core/DmsParser.h
    src/core/CapsImageLoader.cpp
    src/core/CapsImageLoader.h
    src/core/AmigaMfmDecoder.cpp
    src/core/AmigaMfmDecoder.h
    src/core/DiskProfiler.cpp
    src/core/RegWriteProfiler.cpp
    src/core/RegWriteProfiler.h
    src/core/DiskProfiler.h
    src/core/DiskLogSnapshot.h
    src/core/IpfConverter.cpp
    src/core/IpfConverter.h
    src/core/ExtAdfConverter.cpp
    src/core/ExtAdfConverter.h
    src/core/ScpConverter.cpp
    src/core/ScpConverter.h
    src/core/IpfTypes.h
    src/core/LibdiskClassifier.cpp
    src/core/LibdiskClassifier.h
    src/core/DataTypeManager.cpp
    src/core/DataTypeManager.h
    src/core/DefinitionManager.cpp
    src/core/DefinitionManager.h
    src/core/MemoryDevice.cpp
    src/core/MemoryDevice.h
    src/core/Constants.h
    src/core/Logger.cpp
    src/core/Logger.h
    src/core/ImageLoader.cpp
    src/core/ImageLoader.h
    src/core/AudioEngine.cpp
    src/core/CopperDisassembler.cpp
    src/core/CopperRaster.cpp
    src/core/CopperRaster.h
    src/core/RasterFrameMap.h
    src/core/AudioEngine.h
    src/core/AudioModule.cpp
    src/core/AudioModule.h
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/AudioCushion.h
    src/core/WasapiPushOutput.cpp
    src/core/WasapiPushOutput.h
    src/core/HunkParser.cpp
    src/core/HunkParser.h
    src/core/DiskValidator.cpp
    src/core/DiskValidator.h
    src/core/BrainFile.cpp
    src/core/BrainFile.h
    src/core/BootBlockLibrary.cpp
    src/core/BootBlockLibrary.h
    src/core/DiskIdentifier.cpp
    src/core/DiskIdentifier.h
    src/core/GreaseweazelWriter.cpp
    src/core/GreaseweazelWriter.h
    src/core/GreaseweazelReader.cpp
    src/core/GreaseweazelReader.h
    src/core/DiskCompare.cpp
    src/core/DiskCompare.h
    src/core/Subprocess.cpp
    src/core/Subprocess.h
    src/core/FileOperationService.cpp
    src/core/FileOperationService.h
    src/core/SafeReader.h
    src/core/BigEndian.h
    src/core/JsonRead.h
    src/core/FloppyBridgeLimits.h
    src/core/SecureRandom.cpp
    src/core/SecureRandom.h
    src/core/PeekabooEvents.h
    src/core/BlitProfiler.cpp
    src/core/BlitProfiler.h
    src/core/DocumentTransfer.cpp
    src/core/DocumentTransfer.h
    src/core/PeekabooHttpServer.cpp
    src/core/PeekabooHttpServer.h
    src/core/GdbServer.cpp
    src/core/GdbServer.h
    src/core/McpServer.cpp
    src/core/McpServer.h
    src/core/MD5.cpp
    src/core/MD5.h
    src/core/TosecManager.cpp
    src/core/TosecManager.h
    src/core/DownloadManager.cpp
    src/core/DownloadManager.h
    src/core/DiskSearcher.cpp
    src/core/DiskSearcher.h
    src/core/MemorySearcher.cpp
    src/core/MemorySearcher.h
    src/core/DiskFileIndex.cpp
    src/core/DiskFileIndex.h
    src/core/ExecIntrospect.cpp
    src/core/ExecIntrospect.h
    src/core/MemorySnapshotStore.cpp
    src/core/MemorySnapshotStore.h
    src/core/AddressDescriber.cpp
    src/core/AddressDescriber.h
    src/core/LibraryCallTracer.cpp
    src/core/LibraryCallTracer.h
    src/core/ResidentScanner.cpp
    src/core/ResidentScanner.h
    src/core/CpuTraceHistory.cpp
    src/core/CpuTraceHistory.h
    src/core/ByteStream.h
    src/core/FrameDigest.cpp
    src/core/FrameDigest.h
    src/core/FrameInput.h
    src/core/FrameInputCodec.cpp
    src/core/FrameInputCodec.h
    src/core/FrameHistory.cpp
    src/core/FrameHistory.h
    src/core/ScenarioFile.cpp
    src/core/ScenarioFile.h
    src/core/ScenarioRequest.h
    src/core/ScenarioRunner.cpp
    src/core/ScenarioRunner.h
    src/core/FrameThumbnails.cpp
    src/core/FrameThumbnails.h
    src/core/FrameTimeline.cpp
    src/core/FrameTimeline.h
    src/core/BootBlockPreview.cpp
    src/core/BootBlockPreview.h
    src/core/M68kEffectiveAddress.cpp
    src/core/M68kEffectiveAddress.h
    src/core/UaeSavestate.cpp
    src/core/UaeSavestate.h
    src/core/EmulatorStateOps.cpp
    src/core/EmulatorStateOps.h
    src/core/DosBreakpoints.cpp
    src/core/DosBreakpoints.h
    src/core/McpReTools.cpp
    src/core/McpReTools.h
    src/core/McpUiTools.cpp
    src/core/McpUiTools.h
    src/core/UiLayoutSnapshot.h
    src/core/AppCapture.cpp
    src/core/AppCapture.h
    src/core/McpToolHandler.cpp
    src/core/McpToolHandler.h
    src/core/RetroKeyNames.cpp
    src/core/RetroKeyNames.h
    src/core/EmulatorStateStore.cpp
    src/core/EmulatorStateStore.h
    src/core/WatchpointTypes.h
    src/core/McpConsoleTools.cpp
    src/core/McpConsoleTools.h
    src/core/KprintfBreakpoints.cpp
    src/core/KprintfBreakpoints.h
    src/core/debug/SnapshotBuilders.cpp
    src/core/debug/SnapshotBuilders.h
    src/core/validation/ElfSymbolResolver.cpp
    src/core/validation/ElfSymbolResolver.h
    src/core/validation/AmigaValidationMachine.cpp
    src/core/validation/AmigaValidationMachine.h
    src/core/validation/PtvProtocol.cpp
    src/core/validation/PtvProtocol.h
    src/core/validation/SemanticComparator.cpp
    src/core/validation/SemanticComparator.h
    src/core/validation/ValidationSuite.cpp
    src/core/validation/ValidationSuite.h
)
# Note: adflib target is defined in external/adflib/CMakeLists.txt (likely 'adflib' or 'ADFlib')
# We verified external/adflib/CMakeLists.txt defines project(adflib).
# Usually verification of source cmake is required, but standard is 'adflib'.
target_link_libraries(adm_core PUBLIC ZLIB::ZLIB adf fmt::fmt nlohmann_json::nlohmann_json xdms_lib recoil stb_image miniaudio PkgConfig::OPENMPT Crc32c::crc32c cpr::cpr tinyxml2::tinyxml2 glad::glad glfw httplib::httplib ${CMAKE_DL_LIBS})
if(WIN32)
    # SecureRandom.cpp uses BCryptGenRandom for the OS CSPRNG.
    target_link_libraries(adm_core PUBLIC bcrypt winmm ole32)
endif()

# Layering guard: nothing compiled into adm_core may include the ui layer. This cycle existed
# (PeekabooHttpServer.h pulled a view header) and was broken deliberately; fail the configure
# rather than let it grow back. Files that legitimately span both layers (Application, the
# DebugSession/CompiledProgram/LogSnapshots coordinators) live in adm_app instead.
get_target_property(_adm_core_sources adm_core SOURCES)
foreach(_src ${_adm_core_sources})
    if(_src MATCHES "\\.(cpp|h)$" AND EXISTS "${CMAKE_SOURCE_DIR}/${_src}")
        file(STRINGS "${CMAKE_SOURCE_DIR}/${_src}" _ui_includes REGEX "#include \"\\.\\./ui/")
        if(_ui_includes)
            message(FATAL_ERROR "adm_core source ${_src} includes the ui layer:\n${_ui_includes}\n"
                                "Invert the dependency (Ops struct / core-owned type) or move "
                                "the file into adm_app.")
        endif()
    endif()
endforeach()
target_include_directories(adm_core PUBLIC external/TinySHA1)
target_include_directories(adm_core PRIVATE external/gif)
# capsimg (SPS/CAPS) public headers — used header-only; the plugin is dlopen'd at runtime.
target_include_directories(adm_core PUBLIC external/capsimg)
target_include_directories(adm_core PUBLIC src)

# src/Secrets.h is gitignored and machine-local (screenscraper.fr credentials). A fresh clone
# used to fail to compile DiskIdentifier.cpp; fall back to committed empty defaults instead —
# the local file, when present, still wins because the src include dir is searched first.
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/Secrets.h)
    message(STATUS "src/Secrets.h not found — building with empty screenscraper credentials "
                   "(disk identification lookups disabled; see src/Secrets.h.example)")
    configure_file(${CMAKE_SOURCE_DIR}/src/Secrets.h.example
                   ${CMAKE_BINARY_DIR}/generated_secrets/Secrets.h COPYONLY)
    target_include_directories(adm_core PUBLIC ${CMAKE_BINARY_DIR}/generated_secrets)
endif()

# The version is project()'s, in one place: the title bar, About, the console's VERSION and the
# exe's version resource read it from the generated PeekabooVersion.h, and
# scripts/package_peekaboo.ps1 names its zip from the exe's resource.
configure_file(${CMAKE_SOURCE_DIR}/src/core/PeekabooVersion.h.in
               ${CMAKE_BINARY_DIR}/generated_version/PeekabooVersion.h @ONLY)
target_include_directories(adm_core PUBLIC ${CMAKE_BINARY_DIR}/generated_version)

if(ADM_ENABLE_LIBDISK)
    target_link_libraries(adm_core PUBLIC libdisk)
    target_compile_definitions(adm_core PUBLIC ADM_HAVE_LIBDISK)
endif()
target_compile_features(adm_core PUBLIC cxx_std_20)


# --- Main Application ---
set(APP_SOURCES
    src/core/Application.cpp
    src/core/ApplicationDebugServers.cpp
    src/core/Application.h
    src/ui/MainLayout.h
    src/ui/MainLayout.cpp
    src/ui/MenuIcons.h
    src/ui/MenuIcons.cpp
    src/ui/DriveChips.h
    src/ui/DriveChips.cpp
    src/ui/RemoteSession.h
    src/ui/RemoteSession.cpp
    src/ui/PanelManager.h
    src/ui/PanelManager.cpp
    src/ui/ColorManager.h
    src/ui/ColorManager.cpp
    src/ui/ArColorPalette.h
    src/ui/ArColorPalette.cpp
    src/ui/AddressLink.h
    src/ui/AddressLink.cpp
    src/ui/BlitDestPreview.h
    src/ui/BlitDestPreview.cpp
    src/ui/FramePreview.h
    src/ui/FramePreview.cpp
    src/ui/FontManager.h
    src/ui/FontManager.cpp
    src/ui/FontRoles.h
    src/ui/FontRoles.cpp
    src/ui/utils/DefinitionLoader.h
    src/ui/utils/DefinitionLoader.cpp
    src/ui/views/BlockDetailsView.h
    src/ui/views/BlockDetailsView.cpp
    src/ui/views/FileListView.h
    src/ui/views/FileListView.cpp
    src/ui/views/EmulatorView.h
    src/ui/views/EmulatorView.cpp
    src/ui/views/DebuggerView.h
    src/ui/views/DebuggerView.cpp
    src/ui/views/DebuggerCPUView.h
    src/ui/views/DebuggerCPUView.cpp
    src/ui/views/DebuggerCustomView.h
    src/ui/views/DebuggerCustomView.cpp
    src/ui/views/DebuggerDisassemblyView.h
    src/ui/views/DebuggerDisassemblyView.cpp
    src/ui/views/DebuggerCopperView.h
    src/ui/views/DebuggerCopperView.cpp
    src/ui/views/DebuggerMemoryView.h
    src/ui/views/DebuggerMemoryView.cpp
    src/ui/views/DebuggerDisplayView.h
    src/ui/views/DebuggerDisplayView.cpp
    src/ui/views/DebuggerSpritesView.h
    src/ui/views/DebuggerSpritesView.cpp
    src/ui/views/DebuggerBitplanesView.h
    src/ui/views/DebuggerBitplanesView.cpp
    src/ui/views/DebuggerBartmanView.h
    src/ui/views/DebuggerBartmanView.cpp
    src/ui/views/DebuggerCIAView.h
    src/ui/views/DebuggerCIAView.cpp
    src/ui/views/DebuggerAudioView.h
    src/ui/views/DebuggerAudioView.cpp
    src/ui/views/AudioChannelsView.h
    src/ui/views/AudioChannelsView.cpp
    src/ui/views/DebuggerCallStackView.h
    src/ui/views/DebuggerCallStackView.cpp
    src/ui/views/DebuggerTrainerView.h
    src/ui/views/DebuggerTrainerView.cpp
    src/ui/views/SpritesView.h
    src/ui/views/SpritesView.cpp
    src/ui/views/BlitterVisOverlay.h
    src/ui/views/BlitterVisOverlay.cpp
    src/ui/views/CopperRasterOverlay.h
    src/ui/views/CopperRasterOverlay.cpp
    src/ui/views/BitplanesView.h
    src/ui/views/BitplanesView.cpp
    src/ui/views/MemoryMapView.h
    src/ui/views/MemoryMapView.cpp
    src/core/LogSnapshots.cpp
    src/core/LogSnapshots.h
    src/core/DebugSession.cpp
    src/core/DebugSession.h
    src/core/CompiledProgram.cpp
    src/core/CompiledProgram.h
    src/core/RegLogSnapshot.h
    src/ui/views/RegisterOpsView.h
    src/ui/views/RegisterOpsView.cpp
    src/core/BitplaneLayout.h
    src/core/AmigaScreenMap.h
    src/core/BlitScreenMapper.h
    src/core/BlitMemoryMapper.h
    src/core/CpuProfiler.h
    src/core/CpuProfiler.cpp
    src/ui/views/BlitterView.h
    src/ui/views/BlitterView.cpp
    src/ui/views/DiskIoView.h
    src/ui/views/DiskIoView.cpp
    src/ui/views/ExocetMemoryView.cpp
    src/ui/views/ExocetMemoryView.h
    src/ui/views/ExocetControlView.cpp
    src/ui/views/ExocetControlView.h
    src/ui/views/ExocetSong.cpp
    src/ui/views/ExocetSong.h
    src/ui/views/ExocetTimeline.cpp
    src/ui/views/ExocetTimeline.h
    src/ui/ExocetVideoComposer.cpp
    src/ui/ExocetVideoComposer.h
    src/ui/views/ExocetDiskMapView.cpp
    src/ui/views/ExocetDiskMapView.h
    src/ui/views/ExocetMusicView.cpp
    src/ui/views/ExocetMusicView.h
    src/ui/views/ExocetUi.h
    src/ui/views/ExocetView.cpp
    src/ui/views/BlitterMintermView.h
    src/ui/views/BlitterMintermView.cpp
    src/ui/views/DmaStatsView.cpp
    src/ui/views/DebuggerFrameScrubberView.h
    src/ui/views/DebuggerFrameScrubberView.cpp
    src/ui/views/ARConsoleTerminal.h
    src/ui/views/ARConsoleTerminal.cpp
    src/ui/views/ARConsoleView.h
    src/ui/views/ARConsoleView.cpp
    src/ui/views/BreakpointsView.h
    src/ui/views/BreakpointsView.cpp
    src/ui/views/MemoryAnnotationsView.h
    src/ui/views/MemoryAnnotationsView.cpp
    src/ui/views/BlockEditorView.h
    src/ui/views/BlockEditorView.cpp
    src/ui/views/BlockEditorBase.h
    src/ui/views/BlockEditorBase.cpp
    src/ui/views/BlockGridViewBase.h
    src/ui/views/BlockGridViewBase.cpp
    src/ui/views/BlockMapView.h
    src/ui/views/BlockMapView.cpp
    src/ui/views/NDOSView.h
    src/ui/views/NDOSView.cpp
    src/ui/views/BitmapView.h
    src/ui/views/BitmapView.cpp
    src/ui/views/ChangeMapView.h
    src/ui/views/ChangeMapView.cpp
    src/ui/views/FileViewer.h
    src/ui/views/FileViewer.cpp
    src/ui/views/DmsView.h
    src/ui/views/DmsView.cpp
    src/ui/views/TrackView.h
    src/ui/views/TrackView.cpp
    src/ui/views/ImageViewer.h
    src/ui/views/ImageViewer.cpp
    src/ui/views/AudioPlayer.h
    src/ui/views/AudioPlayer.cpp
    src/ui/views/HunkViewer.h
    src/ui/views/HunkViewer.cpp
    src/ui/views/ValidationView.h
    src/ui/views/ValidationView.cpp
    src/ui/views/BootBlockView.h
    src/ui/views/BootBlockView.cpp
    src/ui/views/AboutView.h
    src/ui/views/AboutView.cpp
    src/ui/views/PreferencesView.h
    src/ui/views/PreferencesView.cpp
    src/ui/views/SystemFilesFinder.h
    src/ui/views/SystemFilesFinder.cpp
    src/ui/views/OnboardingView.h
    src/ui/views/OnboardingView.cpp
    src/ui/views/IdentifyView.h
    src/ui/views/IdentifyView.cpp
    src/ui/views/IdentifyResultPanel.h
    src/ui/views/IdentifyResultPanel.cpp
    src/ui/views/TosecBrowserView.h
    src/ui/views/TosecBrowserView.cpp
    src/ui/views/FloppyDiskView.h
    src/ui/views/FloppyDiskView.cpp
    src/ui/views/FloppyDrivesView.cpp
    src/ui/views/LogView.h
    src/ui/views/LogView.cpp
    src/ui/views/PanelView.h
    src/ui/views/PanelView.cpp
    src/ui/TextureManager.h
    src/ui/TextureManager.cpp
    src/ui/WorkspaceTransitionManager.h
    src/ui/WorkspaceTransitionManager.cpp
    src/ui/EmulatorFilter.h
    src/ui/EmulatorFilter.cpp
    src/ui/UiIntrospect.h
    src/ui/UiIntrospect.cpp
    src/ui/UiInputInjector.h
    src/ui/UiInputInjector.cpp
    src/ui/BlitOverlayController.h
    src/ui/BlitOverlayController.cpp
    src/ui/CopperRasterController.h
    src/ui/CopperRasterController.cpp
    src/ui/UnsavedChangesDialog.h
    src/ui/UnsavedChangesDialog.cpp
    src/ui/ViewerLauncher.h
    src/ui/ViewerLauncher.cpp
    src/ui/WorkspaceLayout.h
    src/ui/WorkspaceLayout.cpp
    src/ui/TableLayout.h
    src/ui/TableLayout.cpp
    src/vendor/imgui_hex/imgui_hex.cpp
    src/vendor/imgui_text_edit/TextEditor.cpp
    src/ui/views/AdvancedFindView.cpp
    external/imgui_md/imgui_md.cpp
    src/ui/views/McpActivityView.h
    src/ui/views/McpActivityView.cpp
)

# Application + the whole ui layer as a static library: APP_SOURCES used to live only inside
# the executable, which left ~7,800 lines of the most central logic (Application.cpp alone)
# with no linkable test surface. unit_tests may now link adm_app when a test needs it.
add_library(adm_app STATIC
    ${APP_SOURCES}
)

target_include_directories(adm_app PUBLIC
    src/vendor/imgui_hex
    src/vendor/imgui_text_edit
    external/imgui_md
)

target_link_libraries(adm_app PUBLIC
    imgui::imgui
    glfw
    glad::glad
    adm_core
    nfd::nfd
    nlohmann_json::nlohmann_json
    libzip::zip
    stb_image
    nanosvg
    md4c::md4c
    httplib::httplib
)

set(EXE_SOURCES src/main.cpp)
if(WIN32)
    list(APPEND EXE_SOURCES src/peekaboo.rc)
endif()

add_executable(peekaboo
    ${EXE_SOURCES}
)

target_link_libraries(peekaboo PRIVATE adm_app)

if(APPLE)
    target_link_libraries(peekaboo PRIVATE "-framework Cocoa")
endif()

# Strict warning flags for better code quality — one INTERFACE target linked by every
# first-party target (the test executables used to compile with default warnings, including
# the two server sources they rebuild from APP_SOURCES).
add_library(adm_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(adm_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()
if(MSVC)
    target_compile_options(adm_warnings INTERFACE /W4)
endif()
target_link_libraries(adm_core PRIVATE adm_warnings)
target_link_libraries(adm_app PRIVATE adm_warnings)
target_link_libraries(peekaboo PRIVATE adm_warnings)

# Copy assets to build directory
add_custom_command(TARGET peekaboo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:peekaboo>/assets
    COMMENT "Copying assets to build directory"
)

# Copy datatypes to build directory
add_custom_command(TARGET peekaboo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/datatypes
        $<TARGET_FILE_DIR:peekaboo>/datatypes
    COMMENT "Copying datatypes to build directory"
)

# Copy images to build directory
add_custom_command(TARGET peekaboo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/images
        $<TARGET_FILE_DIR:peekaboo>/images
    COMMENT "Copying images to build directory"
)

# Copy emulation files (ROMs) to build directory
add_custom_command(TARGET peekaboo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/emulation
        $<TARGET_FILE_DIR:peekaboo>/emulation
    COMMENT "Copying emulation files to build directory"
)

# Track the patched PuAE core independently of the executable. A POST_BUILD command
# does not run when only the source DLL changes, which can otherwise leave a stale
# core in an existing build directory after scripts/build_puae.* is run.
# The core file name must match AmigaEmulator::GetDefaultCorePath(): .dll on
# Windows, .dylib everywhere else.
if(WIN32)
    set(PUAE_CORE_NAME "puae_libretro.dll")
else()
    set(PUAE_CORE_NAME "puae_libretro.dylib")
endif()
set(PUAE_CORE_SOURCE "${CMAKE_SOURCE_DIR}/emulation/${PUAE_CORE_NAME}")
set(PUAE_CORE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/emulation/${PUAE_CORE_NAME}")
# The core DLL is gitignored (built by scripts/build_puae.*), so a fresh clone / CI runner has
# no ${PUAE_CORE_SOURCE} and ninja used to fail with "missing and no known rule to make it".
# Deploy only when the DLL exists; emulator-dependent tests carry labels so CI can exclude
# them (ctest -LE emulator-real).
if(EXISTS "${PUAE_CORE_SOURCE}")
    add_custom_command(
        OUTPUT "${PUAE_CORE_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/emulation"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PUAE_CORE_SOURCE}" "${PUAE_CORE_OUTPUT}"
        DEPENDS "${PUAE_CORE_SOURCE}"
        COMMENT "Deploying patched PuAE core"
    )
    add_custom_target(deploy_puae_core ALL DEPENDS "${PUAE_CORE_OUTPUT}")
else()
    message(STATUS "emulation/${PUAE_CORE_NAME} not found — building without the PuAE core "
                   "(run scripts/build_puae.* to produce it; emulator features need it)")
    add_custom_target(deploy_puae_core)
endif()
add_dependencies(peekaboo deploy_puae_core)

# Copy TOSEC data to build directory.
# Named files rather than the whole folder: tosec/ also holds the build-time inputs
# (the source DATs, the Turran FTP index, the pre-2026 backup under archive/), which
# together run to several hundred MB and are not needed at runtime.
set(TOSEC_RUNTIME_FILES
    tosec.json
    categories.json
    hacked.json
    modified.json
)
# Driven by a custom target rather than POST_BUILD so that regenerating tosec.json
# (scripts in tosec/) redeploys it on the next build. A POST_BUILD step only runs
# when the target relinks, which a data-only change does not trigger — the stale
# database would be picked up silently.
set(_tosec_deployed "")
foreach(_tosec_file IN LISTS TOSEC_RUNTIME_FILES)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tosec/${_tosec_file})
        set(_tosec_dst $<TARGET_FILE_DIR:peekaboo>/tosec/${_tosec_file})
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tosec_stamp/${_tosec_file}.stamp
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/tosec/${_tosec_file} ${_tosec_dst}
            COMMAND ${CMAKE_COMMAND} -E touch
                ${CMAKE_CURRENT_BINARY_DIR}/tosec_stamp/${_tosec_file}.stamp
            DEPENDS ${CMAKE_SOURCE_DIR}/tosec/${_tosec_file}
            COMMENT "Copying tosec/${_tosec_file} to build directory"
            VERBATIM
        )
        list(APPEND _tosec_deployed ${CMAKE_CURRENT_BINARY_DIR}/tosec_stamp/${_tosec_file}.stamp)
    endif()
endforeach()
add_custom_target(deploy_tosec_data DEPENDS ${_tosec_deployed})
add_dependencies(peekaboo deploy_tosec_data)

# Copy boot block library + brainfile to build directory (the brainfile.json
# database lives alongside the .bin library and is resolved from this folder).
add_custom_command(TARGET peekaboo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/bootblocks
        $<TARGET_FILE_DIR:peekaboo>/bootblocks
    COMMENT "Copying boot blocks to build directory"
)

# On MinGW, copy runtime DLLs next to the executable so it can run outside of
# the MSYS2 shell (e.g. from an IDE run button). Git for Windows ships its own
# incompatible copies of these DLLs which would otherwise be found first.
if(MINGW)
    get_filename_component(_MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        add_custom_command(TARGET peekaboo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_MINGW_BIN_DIR}/${_dll}"
                $<TARGET_FILE_DIR:peekaboo>
            COMMENT "Copying ${_dll}"
        )
    endforeach()
endif()

# --- Testing ---
enable_testing()

find_package(GTest CONFIG REQUIRED)

add_executable(unit_tests 
    tests/main_test.cpp 
    tests/AdfWrapperTest.cpp
    tests/AdfPartitionTest.cpp
    tests/DefinitionManagerTest.cpp
    tests/BrokenChainTest.cpp
    tests/ZipSupportTest.cpp
    tests/DmsParserTest.cpp
    tests/AmigaMfmDecoderTest.cpp
    tests/DiskProfilerTest.cpp
    tests/RegWriteProfilerTest.cpp
    tests/ArMemoryAccessTest.cpp
    tests/ExtAdfConverterTest.cpp
    tests/ScpConverterTest.cpp
    tests/LibdiskClassifierTest.cpp
    tests/SafeReaderTest.cpp
    tests/DiskCompareTest.cpp
    tests/SecureRandomTest.cpp
    tests/DiskIdentifierTest.cpp
    tests/DisassemblerTest.cpp
    tests/AsmHighlighterTest.cpp
    tests/CSourceHighlighterTest.cpp
    tests/EditorLauncherTest.cpp
    tests/SourceFileCacheTest.cpp
    tests/ArCommandInterpreterTest.cpp
    tests/LibraryCallAnnotatorTest.cpp
    tests/BlitScreenMapperTest.cpp
    tests/BlitDestImageTest.cpp
    tests/BlitterVisOverlayTest.cpp
    tests/AdmBlitVisTest.cpp
    tests/AdmProtectTest.cpp
    tests/MemoryLocksTest.cpp
    tests/TrainerFileTest.cpp
    tests/BitplaneDecoderTest.cpp
    tests/AudioRecorderTest.cpp
    tests/AudioCushionTest.cpp
    tests/PaulaSnapshotTest.cpp
    tests/BlitMemoryMapperTest.cpp
    tests/M68kCyclesTest.cpp
    tests/McpToolHandlerTest.cpp
    tests/BreakpointCommandTest.cpp
    tests/KprintfBreakpointsTest.cpp
    tests/DebugSymbolsTest.cpp
    tests/ElfHunkMapTest.cpp
    tests/LineCoverageTest.cpp
    tests/TimeTravelTest.cpp
    tests/GdbPacketHandlerTest.cpp
    tests/GdbServerTest.cpp
    tests/SegListResolverTest.cpp
    tests/FunctionProfilerTest.cpp
    tests/DebugLinesTest.cpp
    tests/CallStackProfilerTest.cpp
    tests/DebugInlineTest.cpp
    tests/DebugFrameTest.cpp
    tests/StackUnwinderTest.cpp
    tests/LoggerTest.cpp
    tests/PeekabooHttpServerTest.cpp
    tests/McpServerCorsTest.cpp
    tests/HostExecutableLauncherTest.cpp
    tests/HardfileInspectorTest.cpp
    tests/WhdloadLaunchTest.cpp
    tests/CdMediaTest.cpp
    tests/MachineConfigTest.cpp
    tests/CpuExtStateTest.cpp
    tests/HostPortsTest.cpp
    tests/AmigaModelConfigurationTest.cpp
    tests/ExceptionBreakTest.cpp
    tests/EndFrameTest.cpp
    tests/KickstartRomsTest.cpp
    tests/SystemFilesTest.cpp
    tests/KickstartExtRomTest.cpp
    tests/RtgBoardTest.cpp
    tests/AviRecorderTest.cpp
    tests/ZmbvEncoderTest.cpp
    tests/FfmpegRecorderTest.cpp
    tests/AviCaptureTest.cpp
    tests/CopyFileToGuestTest.cpp
    tests/HostDirectoryDocumentTest.cpp
    tests/SessionSerializerTest.cpp
    tests/ValidationFrameworkTest.cpp
    tests/ExecIntrospectTest.cpp
    tests/DosBreakpointsTest.cpp
    tests/McpReToolsTest.cpp
    tests/McpUiToolsTest.cpp
    tests/MemorySnapshotStoreTest.cpp
    tests/AddressDescriberTest.cpp
    tests/LibraryCallTracerTest.cpp
    tests/ResidentScannerTest.cpp
    tests/CpuTraceHistoryTest.cpp
    tests/FrameHistoryTest.cpp
    tests/ScenarioFileTest.cpp
    tests/FrameDigestTest.cpp
    tests/FrameThumbnailsTest.cpp
    tests/M68kEffectiveAddressTest.cpp
    tests/UaeSavestateTest.cpp
    tests/DiskFileIndexTest.cpp
    tests/DiskSearcherTest.cpp
    tests/EmulatorStateStoreTest.cpp
    tests/RetroKeyNamesTest.cpp
    tests/EmulatorWarmResetTest.cpp
    tests/CopperRasterTest.cpp
    tests/FloppySoundTest.cpp
    tests/FontPrefsTest.cpp
    tests/TableLayoutTest.cpp
    tests/ScreenProfileTest.cpp
    tests/EmulatorOptionsTest.cpp
    tests/CrassProfileTest.cpp
    tests/CrassMemMapTest.cpp
    tests/ExocetBlockTest.cpp
    tests/ExocetActivityTest.cpp
    tests/ExocetEvalTest.cpp
    tests/ExocetKitTest.cpp
    tests/ExocetPackTest.cpp
    tests/ExocetDiskMapTest.cpp
    tests/ExocetQueryTest.cpp
    tests/JsonReadTest.cpp
    tests/WorkspaceWindowStatesTest.cpp
    tests/SubprocessTest.cpp
    # Under test, not a whole-ui dependency: TableLayout's ini text handling is pure string
    # work, so the one source is compiled in directly rather than linking all of adm_app.
    src/ui/TableLayout.cpp
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest
    GTest::gtest_main
    adm_core
    adm_warnings
    libzip::zip
    httplib::httplib
    imgui::imgui
)

include(GoogleTest)
# Use PRE_TEST discovery on MinGW to avoid DLL resolution issues at build time
# (Git for Windows can shadow MinGW runtime DLLs when CMake spawns cmd.exe)
if(MINGW)
    gtest_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(unit_tests)
endif()

add_executable(emulator_validation_tests
    tests/emulator_validation/ValidationMain.cpp
)
target_link_libraries(emulator_validation_tests PRIVATE adm_core adm_warnings)
add_dependencies(emulator_validation_tests deploy_puae_core)

# Scenario record/replay and the emulator smoke harness. Headless:
# both digest taps live in the host-side libretro callbacks, so it needs no GL context and
# nothing from the audio device. It still opens one - LoadADF() calls InitAudio() - so the
# harness mutes the machine; see the note in SmokeMain.cpp.
add_executable(peekaboo_smoke
    tests/smoke/SmokeMain.cpp
)
target_link_libraries(peekaboo_smoke PRIVATE adm_core adm_warnings)
add_dependencies(peekaboo_smoke deploy_puae_core)

add_test(NAME emulator_validation_harness_phases_1_5
    COMMAND emulator_validation_tests
        --manifest ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/expected.json
        --profile a500-pal
        --min-phase 1
        --max-phase 5
        --artifacts ${CMAKE_BINARY_DIR}/validation-artifacts/a500-pal
)
set_tests_properties(emulator_validation_harness_phases_1_5 PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "emulator-smoke;emulator-pal"
)

# Needs the real core and the built validation program (peekaboo_validation.exe is a build
# product, not in the repo), so it is registered only where both are there - a fresh checkout
# has neither.
if(EXISTS "${PUAE_CORE_SOURCE}" AND
   EXISTS "${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/peekaboo_validation.exe")
    add_test(NAME emulator_validation_real_phase1
        COMMAND emulator_validation_tests
            --manifest ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/expected.json
            --profile a500-pal
            --min-phase 1
            --max-phase 1
            --repetitions 3
            --artifacts ${CMAKE_BINARY_DIR}/validation-artifacts/real-phase1
            --real
            --core ${PUAE_CORE_OUTPUT}
            --system-dir ${CMAKE_SOURCE_DIR}/emulation
            --target ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/peekaboo_validation.exe
            --elf ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/peekaboo_validation.elf
            --frame-budget 900
    )
    set_tests_properties(emulator_validation_real_phase1 PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LABELS "emulator-real;emulator-pal;emulator-phase1"
        TIMEOUT 120
    )
endif()

add_test(NAME emulator_validation_ntsc
    COMMAND emulator_validation_tests
        --manifest ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/expected.json
        --profile a500-ntsc
        --min-phase 2
        --max-phase 2
        --artifacts ${CMAKE_BINARY_DIR}/validation-artifacts/a500-ntsc
)
set_tests_properties(emulator_validation_ntsc PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "emulator-smoke;emulator-ntsc"
)

add_test(NAME emulator_validation_visual
    COMMAND emulator_validation_tests
        --manifest ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/expected.json
        --profile a500-pal
        --min-phase 4
        --max-phase 4
        --artifacts ${CMAKE_BINARY_DIR}/validation-artifacts/visual
)
set_tests_properties(emulator_validation_visual PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "emulator-visual"
)

# Scenario smoke test: cold-boot the validation ADF for 900 frames and check every frame's
# video and audio digest, plus nine RAM digests, against the blessed goldens. ~3.3 s.
#
# This one needs the real core, so it carries the emulator-real label CI excludes with
# ctest -LE emulator-real, and it is only registered when the core DLL is actually present.
#
# A failure here after a core rebuild is the harness working, not the harness broken: it
# means the machine's output changed. Look at the artifacts, decide whether the change was
# intended, and re-bless with
#   peekaboo_smoke bless tests/smoke/fixtures/boot-a500-pal.pkscn --ram-interval 100 #       --keyframe-interval 300 --adf tests/amiga_validation/fixtures/peekaboo_validation.adf
if(EXISTS "${PUAE_CORE_SOURCE}")
    add_test(NAME emulator_scenario_boot_a500_pal
        COMMAND peekaboo_smoke verify
            ${CMAKE_SOURCE_DIR}/tests/smoke/fixtures/boot-a500-pal.pkscn
            --core ${PUAE_CORE_OUTPUT}
            --system-dir ${CMAKE_SOURCE_DIR}/emulation
            --adf ${CMAKE_SOURCE_DIR}/tests/amiga_validation/fixtures/peekaboo_validation.adf
            --ram-interval 100
            --artifacts ${CMAKE_BINARY_DIR}/smoke-artifacts
            --quiet
    )
    set_tests_properties(emulator_scenario_boot_a500_pal PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LABELS "emulator-real;emulator-scenario;emulator-pal"
        TIMEOUT 120
    )

    # A demo instead of a boot: Enchanted Glitch - Cosmic Orbs, 5000 frames. Covers far more
    # of the machine than the boot does - 3959 distinct video digests and 4631 distinct audio
    # digests out of 5000 frames, against the boot's 18 and 110 - because every frame of a
    # demo is different and the music never stops. The demo is not ours to publish, so it is
    # not in the repo: the test runs where tests/smoke/fixtures/local/ has it.
    set(COSMICORBS_ADF ${CMAKE_SOURCE_DIR}/tests/smoke/fixtures/local/enchantedglitch-cosmicorbs.adf)
    if(EXISTS "${COSMICORBS_ADF}")
        add_test(NAME emulator_scenario_cosmicorbs_a500_pal
            COMMAND peekaboo_smoke verify
                ${CMAKE_SOURCE_DIR}/tests/smoke/fixtures/cosmicorbs-a500-pal.pkscn
                --core ${PUAE_CORE_OUTPUT}
                --system-dir ${CMAKE_SOURCE_DIR}/emulation
                --adf ${COSMICORBS_ADF}
                --ram-interval 250
                --artifacts ${CMAKE_BINARY_DIR}/smoke-artifacts
                --quiet
        )
        # emulator-slow as well, because 26 s is worth being able to exclude on its own.
        set_tests_properties(emulator_scenario_cosmicorbs_a500_pal PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "emulator-real;emulator-scenario;emulator-slow;emulator-pal"
            TIMEOUT 300
        )
    endif()
endif()
