#include "BitplaneDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace adm::core {
namespace {

constexpr int kMaxWidth = 2048;
constexpr int kMaxHeight = 576;

void AddDiagnostic(BitplaneDecodeResult& result, BitplaneDiagnosticSeverity severity,
                   std::string message, int plane = -1, uint32_t address = 0)
{
    result.diagnostics.push_back({severity, std::move(message), plane, address});
}

int ActivePlaneCount(uint16_t bplcon0)
{
    int count = (bplcon0 >> 12) & 0x7;
    if (bplcon0 & 0x0010)
        count |= 0x8;
    return std::clamp(count, 0, BitplaneLayout::kMaxPlanes);
}

BitplaneDisplayMode ResolveDisplayMode(uint16_t bplcon0, int planes)
{
    if (bplcon0 & 0x0800)
        return planes >= 8 ? BitplaneDisplayMode::Ham8 : BitplaneDisplayMode::Ham6;
    if (bplcon0 & 0x0400)
        return BitplaneDisplayMode::DualPlayfield;
    if (planes == 6)
        return BitplaneDisplayMode::ExtraHalfBrite;
    return BitplaneDisplayMode::Indexed;
}

int ResolveHeight(const Custom& regs, bool& usedFallback)
{
    int start = (regs.diwstrt >> 8) & 0xFF;
    int stop = (regs.diwstop >> 8) & 0xFF;

    // ECS/AGA extend both positions to 11 bits. A zero DIWHIGH is also the normal OCS state.
    if (regs.diwhigh != 0) {
        start |= (regs.diwhigh & 0x0007) << 8;
        stop |= ((regs.diwhigh >> 8) & 0x0007) << 8;
    } else if (stop <= start) {
        stop += 256;
    }

    const int height = stop - start;
    usedFallback = height <= 0 || height > kMaxHeight;
    return usedFallback ? 256 : height;
}

int ResolveFetchWidth(const Custom& regs, BitplaneResolution resolution, bool& usedFallback)
{
    const int width = FetchWidthPx(regs.ddfstrt, regs.ddfstop, resolution);
    usedFallback = width < 16 || width > kMaxWidth;
    return usedFallback ? 320 : width;
}

int ResolveVisibleWidth(const Custom& regs, BitplaneResolution resolution)
{
    // DIW low bytes contain H9..H2 in super-hires units. Without DIWHIGH, the stop
    // position has the classic implicit H10 bit. ECS/AGA supply H10..H0 explicitly.
    int start = (regs.diwstrt & 0xFF) << 2;
    int stop = (regs.diwstop & 0xFF) << 2;
    if (regs.diwhigh != 0) {
        start |= (regs.diwhigh >> 3) & 0x3;
        start |= ((regs.diwhigh >> 5) & 0x1) << 10;
        stop |= (regs.diwhigh >> 11) & 0x3;
        stop |= ((regs.diwhigh >> 13) & 0x1) << 10;
        if (stop <= start)
            stop += 2048;
    } else {
        stop |= 1 << 10;
    }

    int divisor = 4;
    if (resolution == BitplaneResolution::Hires)
        divisor = 2;
    else if (resolution == BitplaneResolution::SuperHires)
        divisor = 1;
    const int width = (stop - start) / divisor;
    return (width >= 16 && width <= kMaxWidth) ? width : 0;
}

const BitplaneMemoryView* FindMemory(const std::vector<BitplaneMemoryView>& memory,
                                     uint32_t address)
{
    for (const auto& region : memory) {
        if (region.data && address >= region.amigaAddress &&
            static_cast<uint64_t>(address) <
                static_cast<uint64_t>(region.amigaAddress) + region.size) {
            return &region;
        }
    }
    return nullptr;
}

bool ReadByte(const std::vector<BitplaneMemoryView>& memory, uint32_t address, uint8_t& value)
{
    const auto* region = FindMemory(memory, address);
    if (!region) {
        value = 0;
        return false;
    }
    value = region->data[address - region->amigaAddress];
    return true;
}

} // namespace

bool BitplaneDecodeResult::HasWarnings() const
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == BitplaneDiagnosticSeverity::Warning;
    });
}

bool BitplaneDecodeResult::HasErrors() const
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == BitplaneDiagnosticSeverity::Error;
    });
}

BitplaneDecodeResult BitplaneDecoder::Decode(const Custom& regs,
                                             const BitplaneDecodeOptions& options,
                                             const std::vector<BitplaneMemoryView>& memory,
                                             bool decodePixels)
{
    BitplaneDecodeResult result;
    auto& layout = result.layout;

    layout.resolution = ResolutionFromBplcon0(regs.bplcon0);
    layout.interlaced = (regs.bplcon0 & 0x0004) != 0;
    layout.bplCount = options.automatic
                          ? ActivePlaneCount(regs.bplcon0)
                          : std::clamp(options.manualPlaneCount, 1, BitplaneLayout::kMaxPlanes);
    layout.displayMode = ResolveDisplayMode(regs.bplcon0, layout.bplCount);

    bool widthFallback = false;
    bool heightFallback = false;
    int detectedWidth = ResolveFetchWidth(regs, layout.resolution, widthFallback);
    int detectedHeight = ResolveHeight(regs, heightFallback);
    layout.visibleWidth = ResolveVisibleWidth(regs, layout.resolution);
    layout.fetchUnitBytes = (regs.fmode & 0x0001) ? 4 : 2;

    for (int plane = 0; plane < BitplaneLayout::kMaxPlanes; ++plane) {
        const uint32_t registerPointer = SwapPtrWords(regs.bplpt[plane]);
        layout.bplBase[plane] = options.useRegisterPointers
                                    ? registerPointer
                                    : options.manualPointers[static_cast<size_t>(plane)];
    }

    bool detectedInterleaved = false;
    if (layout.bplCount >= 2 && layout.bplBase[0] != 0 && layout.bplBase[1] != 0) {
        const uint32_t distance =
            layout.bplBase[1] > layout.bplBase[0] ? layout.bplBase[1] - layout.bplBase[0] : 0;
        if (distance > 0 && distance <= 512) {
            const int modulo1 = static_cast<int>(static_cast<int16_t>(regs.bpl1mod));
            const bool moduloMatches =
                modulo1 == (layout.bplCount - 1) * static_cast<int>(distance);
            const bool pointersMatch =
                layout.bplCount >= 3 && layout.bplBase[2] == layout.bplBase[1] + distance;
            if (moduloMatches || pointersMatch) {
                const int pointerWidth = static_cast<int>(distance) * 8;
                if (detectedWidth < 256 || std::abs(pointerWidth - detectedWidth) <= 16)
                    detectedWidth = pointerWidth;
                detectedInterleaved = true;
            } else if (detectedHeight > 0 && distance % detectedHeight == 0) {
                const int stride = static_cast<int>(distance) / detectedHeight;
                if (std::abs(stride * 8 - detectedWidth) <= 16)
                    detectedWidth = stride * 8;
            }
        }
    }

    if (options.automatic) {
        layout.screenWidth = options.overrideWidth ? options.manualWidth : detectedWidth;
        layout.screenHeight = options.overrideHeight ? options.manualHeight : detectedHeight;
        result.provenance.width =
            options.overrideWidth
                ? BitplaneValueSource::Manual
                : (widthFallback ? BitplaneValueSource::Fallback : BitplaneValueSource::Derived);
        result.provenance.height =
            options.overrideHeight
                ? BitplaneValueSource::Manual
                : (heightFallback ? BitplaneValueSource::Fallback : BitplaneValueSource::Derived);
    } else {
        layout.screenWidth = options.manualWidth;
        layout.screenHeight = options.manualHeight;
        result.provenance.planeCount = BitplaneValueSource::Manual;
        result.provenance.width = BitplaneValueSource::Manual;
        result.provenance.height = BitplaneValueSource::Manual;
    }
    layout.screenWidth = std::clamp(layout.screenWidth, 16, kMaxWidth);
    layout.screenWidth = (layout.screenWidth / 16) * 16;
    layout.screenHeight = std::clamp(layout.screenHeight, 1, kMaxHeight);
    layout.fetchWidth = layout.screenWidth;
    if (layout.visibleWidth == 0)
        layout.visibleWidth = layout.fetchWidth;

    layout.interleaved =
        options.automaticInterleaving ? detectedInterleaved : options.forcedInterleaving;
    result.provenance.interleaving =
        options.automaticInterleaving ? BitplaneValueSource::Derived : BitplaneValueSource::Manual;
    result.provenance.pointers =
        options.useRegisterPointers ? BitplaneValueSource::Registers : BitplaneValueSource::Manual;
    const int bytesPerRow = layout.screenWidth / 8;
    const int rowDataBytes = (layout.interleaved ? layout.bplCount : 1) * bytesPerRow;
    const int registerModulo1 = static_cast<int>(static_cast<int16_t>(regs.bpl1mod));
    const int registerModulo2 = static_cast<int>(static_cast<int16_t>(regs.bpl2mod));
    int resolvedModulo1 = options.overrideModulo1 ? options.manualModulo1 : registerModulo1;
    int resolvedModulo2 = options.overrideModulo2 ? options.manualModulo2 : registerModulo2;
    if (layout.interleaved && layout.bplCount > 1) {
        resolvedModulo1 -= (layout.bplCount - 1) * bytesPerRow;
        resolvedModulo2 -= (layout.bplCount - 1) * bytesPerRow;
    }
    layout.modulo1 =
        options.overrideStride1 ? options.manualStride1 - rowDataBytes : resolvedModulo1;
    layout.modulo2 =
        options.overrideStride2 ? options.manualStride2 - rowDataBytes : resolvedModulo2;
    result.provenance.modulo = options.overrideStride1 || options.overrideStride2 ||
                                       options.overrideModulo1 || options.overrideModulo2
                                   ? BitplaneValueSource::Manual
                                   : BitplaneValueSource::Registers;

    layout.ddfStartLoresPx = (regs.ddfstrt & 0xFE) * 2;
    layout.diwVStart = (regs.diwstrt >> 8) & 0xFF;
    if (regs.diwhigh != 0)
        layout.diwVStart |= (regs.diwhigh & 0x0007) << 8;
    layout.scroll1 = regs.bplcon1 & 0x000F;
    layout.scroll2 = (regs.bplcon1 >> 4) & 0x000F;
    layout.valid = layout.bplCount > 0 && layout.screenWidth > 0 && layout.screenHeight > 0;

    if (layout.bplCount == 0) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Info,
                      "No bitplanes are enabled in BPLCON0.");
        return result;
    }
    if ((regs.dmaconr & 0x0100) == 0) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Warning,
                      "Bitplane DMA is disabled; pointers may describe an inactive buffer.");
    }
    if (widthFallback || heightFallback) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Warning,
                      "Invalid DDF/DIW geometry required a standard-size fallback.");
    }
    if (layout.visibleWidth != layout.fetchWidth) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Info,
                      "The DIW display width differs from the fetched data width; the preview "
                      "shows the complete fetch.");
    }
    if (layout.fetchUnitBytes == 4) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Info,
                      "FMODE selects 32-bit bitplane fetches.");
    }
    if (layout.interlaced) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Info,
                      "Interlace is active; the preview represents the currently captured field.");
    }
    if (layout.scroll1 != 0 || layout.scroll2 != 0) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Info,
                      "BPLCON1 fine scroll is present; the preview shows the fetched data extent.");
    }
    if (layout.displayMode != BitplaneDisplayMode::Indexed &&
        layout.displayMode != BitplaneDisplayMode::ExtraHalfBrite) {
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Warning,
                      "This display mode is recognized but its composite preview is approximate.");
    }

    if (!decodePixels)
        return result;

    const int64_t totalPixels = static_cast<int64_t>(layout.screenWidth) * layout.screenHeight;
    if (totalPixels <= 0 || totalPixels > std::numeric_limits<int>::max()) {
        layout.valid = false;
        AddDiagnostic(result, BitplaneDiagnosticSeverity::Error,
                      "Resolved dimensions are too large to decode safely.");
        return result;
    }

    for (int plane = 0; plane < layout.bplCount; ++plane) {
        auto& bits = result.planeBits[static_cast<size_t>(plane)];
        bits.assign(static_cast<size_t>(totalPixels), 0);
        auto& status = result.planeStatus[static_cast<size_t>(plane)];
        status.requestedBytes = static_cast<uint64_t>(bytesPerRow) * layout.screenHeight;

        const uint32_t base = layout.bplBase[plane];
        if (base == 0) {
            AddDiagnostic(result, BitplaneDiagnosticSeverity::Warning,
                          "Bitplane pointer is zero; the plane was not decoded.", plane);
            continue;
        }

        const int stride = layout.StrideBytes(plane);
        if (stride <= 0) {
            AddDiagnostic(result, BitplaneDiagnosticSeverity::Error,
                          "Resolved row stride is zero or negative.", plane, base);
            continue;
        }

        uint32_t firstUnreadable = 0;
        bool foundUnreadable = false;
        for (int y = 0; y < layout.screenHeight; ++y) {
            const uint64_t rowAddress64 = static_cast<uint64_t>(base) +
                                          static_cast<uint64_t>(y) * static_cast<uint64_t>(stride);
            if (rowAddress64 > std::numeric_limits<uint32_t>::max()) {
                if (!foundUnreadable) {
                    firstUnreadable = std::numeric_limits<uint32_t>::max();
                    foundUnreadable = true;
                }
                continue;
            }
            const uint32_t rowAddress = static_cast<uint32_t>(rowAddress64);
            uint8_t* row = bits.data() + static_cast<size_t>(y) * layout.screenWidth;
            // Resolve the region once per row instead of once per byte (an 8-plane 320×256
            // decode is ~164k lookups otherwise). Rows that straddle a region boundary — or
            // miss entirely — fall back to the per-byte path.
            const BitplaneMemoryView* rowRegion = FindMemory(memory, rowAddress);
            const bool rowContiguous =
                rowRegion && static_cast<uint64_t>(rowAddress) + bytesPerRow <=
                                 static_cast<uint64_t>(rowRegion->amigaAddress) + rowRegion->size;
            const uint8_t* rowSrc =
                rowContiguous ? rowRegion->data + (rowAddress - rowRegion->amigaAddress) : nullptr;
            for (int byteIndex = 0; byteIndex < bytesPerRow; ++byteIndex) {
                uint8_t value = 0;
                if (rowSrc) {
                    value = rowSrc[byteIndex];
                    ++status.readableBytes;
                } else {
                    const uint32_t address = rowAddress + static_cast<uint32_t>(byteIndex);
                    if (ReadByte(memory, address, value)) {
                        ++status.readableBytes;
                    } else if (!foundUnreadable) {
                        firstUnreadable = address;
                        foundUnreadable = true;
                    }
                }
                for (int bit = 7; bit >= 0; --bit)
                    row[byteIndex * 8 + (7 - bit)] = (value >> bit) & 1;
            }
        }

        if (status.readableBytes == 0) {
            AddDiagnostic(result, BitplaneDiagnosticSeverity::Error,
                          "No bytes for this plane are present in mapped memory.", plane,
                          firstUnreadable);
        } else if (status.readableBytes < status.requestedBytes) {
            AddDiagnostic(result, BitplaneDiagnosticSeverity::Warning,
                          "Only part of this plane is present in mapped memory.", plane,
                          firstUnreadable);
        }
    }

    result.colorIndices.assign(static_cast<size_t>(totalPixels), 0);
    for (int plane = 0; plane < layout.bplCount; ++plane) {
        const auto& bits = result.planeBits[static_cast<size_t>(plane)];
        if (bits.empty())
            continue;
        for (size_t pixel = 0; pixel < bits.size(); ++pixel) {
            if (bits[pixel])
                result.colorIndices[pixel] |= static_cast<uint8_t>(1u << plane);
        }
    }

    return result;
}

const char* DisplayModeName(BitplaneDisplayMode mode)
{
    switch (mode) {
        case BitplaneDisplayMode::ExtraHalfBrite:
            return "EHB";
        case BitplaneDisplayMode::DualPlayfield:
            return "dual-playfield";
        case BitplaneDisplayMode::Ham6:
            return "HAM6";
        case BitplaneDisplayMode::Ham8:
            return "HAM8";
        case BitplaneDisplayMode::Indexed:
        default:
            return "indexed";
    }
}

ScreenMode DescribeScreenMode(const Custom& regs)
{
    ScreenMode m;
    m.resolution = ResolutionFromBplcon0(regs.bplcon0);
    m.pixelsPerLores = ResolutionPixelsPerLores(m.resolution);
    m.interlaced = (regs.bplcon0 & 0x0004) != 0;
    m.bitplanes = ActivePlaneCount(regs.bplcon0);
    m.displayMode = ResolveDisplayMode(regs.bplcon0, m.bitplanes);
    m.bitplaneDma = (regs.dmaconr & (1 << 8)) != 0;
    m.spriteDma = (regs.dmaconr & (1 << 5)) != 0;

    // ResolveVisibleWidth already reports 0 for a nonsensical DIW; ResolveHeight falls back to
    // 256, which would be a guess here, so surface that as "unknown" (0) instead.
    m.displayWidth = ResolveVisibleWidth(regs, m.resolution);

    bool heightFallback = false;
    const int lines = ResolveHeight(regs, heightFallback);
    m.displayHeight = heightFallback ? 0 : lines * (m.interlaced ? 2 : 1);

    const int fetch = FetchWidthPx(regs.ddfstrt, regs.ddfstop, m.resolution);
    m.fetchWidth = (fetch >= 16 && fetch <= kMaxWidth) ? fetch : 0;
    return m;
}

std::string ScreenModeSummary(const ScreenMode& m)
{
    char buf[128];
    char dims[32] = "?x?";
    if (m.displayWidth > 0 && m.displayHeight > 0)
        std::snprintf(dims, sizeof(dims), "%dx%d", m.displayWidth, m.displayHeight);

    std::snprintf(buf, sizeof(buf), "%s %s%s %dbpl%s%s", dims, ResolutionName(m.resolution),
                  m.interlaced ? " lace" : "", m.bitplanes,
                  m.displayMode == BitplaneDisplayMode::Indexed ? "" : " ",
                  m.displayMode == BitplaneDisplayMode::Indexed ? ""
                                                                : DisplayModeName(m.displayMode));
    return buf;
}

} // namespace adm::core
