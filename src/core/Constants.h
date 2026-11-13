#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "PeekabooVersion.h" // generated from CMakeLists.txt's project() version

namespace adm::core::constants {

// Application
constexpr std::string_view AppName = "PEEKaboo";
constexpr std::string_view AppVersion = PEEKABOO_VERSION;
constexpr std::string_view ConsoleVersion = "0.3";
constexpr int DefaultWindowWidth = 1280;
constexpr int DefaultWindowHeight = 720;

// Paths
constexpr std::string_view AssetsFontDir = "assets/fonts";

// Definitions
namespace definitions {
constexpr std::string_view BootBlock = "definitions/bootblock.json";
constexpr std::string_view RootBlock = "definitions/rootblock.json";
constexpr std::string_view FileHeader = "definitions/fileheader.json";
constexpr std::string_view FileExtension = "definitions/fileextension.json";
constexpr std::string_view UserDirectory = "definitions/userdirectory.json";
constexpr std::string_view DataBlock = "definitions/datablock_ofs.json";
constexpr std::string_view DataBlockFFS = "definitions/datablock_ffs.json";
constexpr std::string_view Bitmap = "definitions/bitmap.json";
} // namespace definitions

// Video beam timing (PAL)
namespace beam {
// Each scanline is 227.5 CCK (color clocks) on average; 228 is used as the integer stride so
// long-frame lines (228 CCK) don't collide with the next line's slot 0. L-reference =
// vpos * CCKPerLine + hpos (shared by the CPU, Copper, and Blitter traces).
constexpr uint32_t CCKPerLine = 228;
constexpr uint32_t TotalCCK = 71364; // 313 lines * 228 CCK per short PAL frame

// Colour clocks an Agnus line actually has: 227 on PAL and on a short NTSC line (custom.c
// maxhpos), one more on a long NTSC line. CCKPerLine above is the L-Ref stride, not this: on PAL
// hpos 227 never happens, and the displayed line wraps into the next Agnus line at 227.
constexpr uint32_t CCKPerAgnusLine = 227;

// Denise's horizontal counter trails the Agnus beam by this many colour clocks. A register
// written at hpos H changes the picture at lores position 2*H - 8 (custom.c hpos_to_diwx,
// DDF_OFFSET), so while the beam is at hpos H it is drawing Denise colour clock H - 4. Measured
// with a Copper colour split on an OCS A500: MOVE COLOR00 at hpos 70 put the colour edge at
// Denise clock 66.0, and at hpos 8 of the next line at Denise clock 231.0 (227 + 8 - 4).
constexpr uint32_t DeniseLagCCK = 4;
} // namespace beam

// Disk geometry
namespace disk {
constexpr uint32_t BlockSize = 512;
constexpr uint32_t DDTotalBlocks = 1760;
constexpr uint32_t DDCylinders = 80;
constexpr uint32_t DDHeads = 2;
constexpr uint32_t DDSectorsPerTrack = 11;
constexpr uint32_t BlocksPerCylinder = 22;
constexpr size_t MaxImportFileSize = 2 * 1024 * 1024; // 2MB safety limit
// Upper bound for a single ADF extracted from a (possibly untrusted, internet-sourced)
// ZIP archive. Guards against decompression bombs that declare an enormous uncompressed
// size in the ZIP header. Generous enough for HD floppies and large hardfile images.
constexpr size_t MaxZipAdfSize = 128 * 1024 * 1024; // 128MB
} // namespace disk

} // namespace adm::core::constants
