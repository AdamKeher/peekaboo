#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "AmigaCustom.h"
#include "BitplaneLayout.h"

namespace adm::core {

enum class BitplaneDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

enum class BitplaneValueSource {
    Registers,
    Derived,
    Manual,
    Fallback,
};

struct BitplaneDecodeProvenance
{
    BitplaneValueSource planeCount = BitplaneValueSource::Registers;
    BitplaneValueSource width = BitplaneValueSource::Derived;
    BitplaneValueSource height = BitplaneValueSource::Derived;
    BitplaneValueSource pointers = BitplaneValueSource::Registers;
    BitplaneValueSource modulo = BitplaneValueSource::Registers;
    BitplaneValueSource interleaving = BitplaneValueSource::Derived;
};

struct BitplaneDiagnostic
{
    BitplaneDiagnosticSeverity severity = BitplaneDiagnosticSeverity::Info;
    std::string message;
    int plane = -1;
    uint32_t address = 0;
};

struct BitplaneMemoryView
{
    const uint8_t* data = nullptr;
    uint32_t amigaAddress = 0;
    uint32_t size = 0;
};

struct BitplaneDecodeOptions
{
    bool automatic = true;
    int manualPlaneCount = 3;
    int manualWidth = 320;
    int manualHeight = 256;

    bool overrideWidth = false;
    bool overrideHeight = false;
    bool overrideModulo1 = false;
    bool overrideModulo2 = false;
    int manualModulo1 = 0;
    int manualModulo2 = 0;

    bool automaticInterleaving = true;
    bool forcedInterleaving = false;

    bool useRegisterPointers = true;
    std::array<uint32_t, BitplaneLayout::kMaxPlanes> manualPointers{};

    bool overrideStride1 = false;
    bool overrideStride2 = false;
    int manualStride1 = 40;
    int manualStride2 = 40;
};

struct BitplanePlaneDecodeStatus
{
    uint64_t requestedBytes = 0;
    uint64_t readableBytes = 0;

    bool complete() const { return requestedBytes > 0 && readableBytes == requestedBytes; }
    bool partial() const { return readableBytes > 0 && readableBytes < requestedBytes; }
};

struct BitplaneDecodeResult
{
    BitplaneLayout layout;
    std::array<std::vector<uint8_t>, BitplaneLayout::kMaxPlanes> planeBits;
    std::vector<uint8_t> colorIndices;
    std::array<BitplanePlaneDecodeStatus, BitplaneLayout::kMaxPlanes> planeStatus{};
    std::vector<BitplaneDiagnostic> diagnostics;
    BitplaneDecodeProvenance provenance;

    bool HasWarnings() const;
    bool HasErrors() const;
};

// Resolves a live/manual bitplane description and optionally decodes every active plane.
// This class has no emulator, ImGui, or OpenGL dependencies, making geometry and recovery
// behaviour deterministic and unit-testable.
class BitplaneDecoder
{
public:
    static BitplaneDecodeResult Decode(const Custom& regs, const BitplaneDecodeOptions& options,
                                       const std::vector<BitplaneMemoryView>& memory,
                                       bool decodePixels = true);
};

// A plain-language summary of "what screen mode is the program running", in the pixels the
// program actually draws rather than in raw register units. Shared by every surface that
// reports the screen mode (Display window overlay, MCP tools, AR console) so they can't
// disagree — the class of bug where DIW's lores units were reported verbatim and a 640x256
// hires screen read as 320x256.
struct ScreenMode
{
    BitplaneResolution resolution = BitplaneResolution::Lores;
    BitplaneDisplayMode displayMode = BitplaneDisplayMode::Indexed;
    bool interlaced = false;
    int bitplanes = 0;      // BPU from BPLCON0, including the AGA 8th-plane bit
    int pixelsPerLores = 1; // 1 / 2 / 4

    int displayWidth = 0;  // DIW window width in native pixels
    int displayHeight = 0; // DIW window height in display lines (doubled when interlaced)
    int fetchWidth = 0;    // DDF fetch width in native pixels

    bool bitplaneDma = false;
    bool spriteDma = false;
};

// Decodes the screen mode from a custom-register snapshot. Never fails; an unconfigured or
// nonsensical register set yields zeroed dimensions rather than guesses.
ScreenMode DescribeScreenMode(const Custom& regs);

// "640x256 hires 4bpl" / "320x256 lores lace 6bpl HAM" — one-line human-readable form.
std::string ScreenModeSummary(const ScreenMode& mode);

const char* DisplayModeName(BitplaneDisplayMode mode);

} // namespace adm::core
