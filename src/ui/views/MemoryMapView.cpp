#include "MemoryMapView.h"
#include "../TableLayout.h"

#include "../UiIntrospect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <glad/glad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "../../core/AmigaCustom.h"
#include "../../core/BitplaneLayout.h"
#include "../../core/BlitMemoryMapper.h"
#include "../AddressLink.h"
#include "../IconsFontAwesome6.h"
#include "../Magnifier.h"
#include "../RegionColors.h"

namespace adm::ui {

MemoryMapView::MemoryMapView(EmulatorView* emulatorView, TextureManager* textureManager)
    : m_emulatorView(emulatorView), m_textureManager(textureManager)
{
    m_previewTexH = kPreviewH;
    m_previewPixels.resize(kPreviewW * m_previewTexH * 4, 0);
    m_renderThread = std::thread(&MemoryMapView::RenderWorkerMain, this);
}

MemoryMapView::~MemoryMapView()
{
    {
        std::lock_guard lock(m_renderMutex);
        m_stopThread = true;
    }
    m_renderCv.notify_one();
    if (m_renderThread.joinable())
        m_renderThread.join();
    if (m_previewTexId != 0)
        m_textureManager->ReleaseTexture(m_previewTexId);
    if (m_mainTexId != 0)
        m_textureManager->ReleaseTexture(m_mainTexId);
    if (m_loupeTexId != 0)
        m_textureManager->ReleaseTexture(m_loupeTexId);
}

// ── Memory helpers ────────────────────────────────────────────────────────────

uint32_t MemoryMapView::GetTotalSpan() const
{
    uint32_t maxEnd = 0;
    for (const auto& r : m_regions) {
        uint32_t end = r.amigaAddr + r.size;
        if (end > maxEnd)
            maxEnd = end;
    }
    return maxEnd == 0 ? 1 : maxEnd;
}

// ── Rendering ─────────────────────────────────────────────────────────────────

// static — no member access; called from the worker thread.
void MemoryMapView::DoRenderPixels(uint8_t* outPixels, int w, int h, int rowBegin, int rowCount,
                                   uint32_t startAddress, int colW,
                                   const std::vector<adm::AmigaEmulator::MemoryRegion>& regions)
{
    if (w <= 0 || h <= 0 || rowCount <= 0 || !outPixels)
        return;

    const int colStride = colW + 1;
    const int numCols = std::max(1, (w + colStride - 1) / colStride);
    const int wordsPerCol = colW / 16;
    const int bytesPerRow = colW / 8;
    const int bytesPerCol = bytesPerRow * h;

    const uint32_t gapColor = (255u << 24) | (8u << 16) | (8u << 8) | 8u;
    const uint32_t unmappedColor = (255u << 24) | (20u << 16) | (20u << 8) | 80u;
    const uint32_t onColor = (255u << 24) | (220u << 16) | (220u << 8) | 220u;
    const uint32_t offColor = (255u << 24) | (40u << 16) | (40u << 8) | 40u;

    uint32_t* allPixels = reinterpret_cast<uint32_t*>(outPixels);
    std::fill(allPixels, allPixels + static_cast<size_t>(w) * rowCount, gapColor);

    // Local region-lookup cache (mirrors m_lastRegionIdx but thread-local).
    int lastRegionIdx = 0;

    auto readWord = [&](uint32_t addr, uint16_t& outWord) -> bool {
        // Fast path: check last used region first.
        if (!regions.empty()) {
            const auto& r0 = regions[lastRegionIdx];
            if (addr >= r0.amigaAddr && addr + 1 < r0.amigaAddr + r0.size) {
                outWord = r0.hostPtr
                              ? static_cast<uint16_t>((r0.hostPtr[addr - r0.amigaAddr] << 8) |
                                                      r0.hostPtr[addr + 1 - r0.amigaAddr])
                              : 0u;
                return true;
            }
        }
        for (int i = 0; i < static_cast<int>(regions.size()); ++i) {
            const auto& r = regions[i];
            if (addr >= r.amigaAddr && addr + 1 < r.amigaAddr + r.size) {
                lastRegionIdx = i;
                outWord = r.hostPtr ? static_cast<uint16_t>((r.hostPtr[addr - r.amigaAddr] << 8) |
                                                            r.hostPtr[addr + 1 - r.amigaAddr])
                                    : 0u;
                return true;
            }
        }
        // Boundary-crossing / unmapped
        bool m0 = false, m1 = false;
        uint8_t b0 = 0, b1 = 0;
        for (const auto& r : regions) {
            if (addr >= r.amigaAddr && addr < r.amigaAddr + r.size) {
                m0 = true;
                b0 = r.hostPtr ? r.hostPtr[addr - r.amigaAddr] : 0;
            }
            if (addr + 1 >= r.amigaAddr && addr + 1 < r.amigaAddr + r.size) {
                m1 = true;
                b1 = r.hostPtr ? r.hostPtr[addr + 1 - r.amigaAddr] : 0;
            }
        }
        outWord = static_cast<uint16_t>((b0 << 8) | b1);
        return m0 || m1;
    };

    for (int col = 0; col < numCols; ++col) {
        const int colX = col * colStride;
        const uint32_t colBase =
            startAddress + static_cast<uint32_t>(col) * static_cast<uint32_t>(bytesPerCol);

        for (int row = rowBegin; row < rowBegin + rowCount; ++row) {
            const uint32_t rowAddr = colBase + static_cast<uint32_t>(row) * bytesPerRow;
            uint32_t* rowPtr = allPixels + static_cast<size_t>(row - rowBegin) * w;

            for (int c = 0; c < wordsPerCol; ++c) {
                int start_px_x = colX + c * 16;
                int max_b = std::min(16, w - start_px_x);
                if (max_b <= 0)
                    break;

                const uint32_t addr = rowAddr + static_cast<uint32_t>(c) * 2;
                uint16_t word = 0;
                const bool mapped = readWord(addr, word);

                uint32_t* pxPtr = rowPtr + start_px_x;
                if (!mapped) {
                    for (int b = 0; b < max_b; ++b)
                        pxPtr[b] = unmappedColor;
                } else {
                    for (int b = 0; b < max_b; ++b)
                        pxPtr[b] = ((word >> (15 - b)) & 1) ? onColor : offColor;
                }
            }
        }
    }
}

// static — worker thread. At 1x/zoomed in the texture is the grid; zoomed out, each texel is the
// average of a downscale x downscale block, so sparse data still reads as a lighter grey instead
// of vanishing between sampled pixels. Rendered one strip of grid rows at a time to bound memory.
void MemoryMapView::RenderRequestPixels(const RenderRequest& request, std::vector<uint8_t>& out)
{
    out.assign(static_cast<size_t>(request.texW) * request.texH * 4, 0);
    const int f = request.downscale;
    if (f <= 1) {
        DoRenderPixels(out.data(), request.gridW, request.gridH, 0, request.gridH,
                       request.startAddress, request.colW, request.regions);
        return;
    }

    std::vector<uint8_t> strip(static_cast<size_t>(request.gridW) * f * 4);
    std::vector<uint32_t> sums(static_cast<size_t>(request.texW) * 3);
    const uint32_t* src = reinterpret_cast<const uint32_t*>(strip.data());
    uint32_t* dst = reinterpret_cast<uint32_t*>(out.data());
    const uint32_t div = static_cast<uint32_t>(f * f);

    for (int ty = 0; ty < request.texH; ++ty) {
        DoRenderPixels(strip.data(), request.gridW, request.gridH, ty * f, f, request.startAddress,
                       request.colW, request.regions);
        std::fill(sums.begin(), sums.end(), 0u);
        for (int r = 0; r < f; ++r) {
            const uint32_t* rowPx = src + static_cast<size_t>(r) * request.gridW;
            for (int gx = 0; gx < request.gridW; ++gx) {
                const uint32_t p = rowPx[gx];
                uint32_t* acc = &sums[static_cast<size_t>(gx / f) * 3];
                acc[0] += p & 0xFF;
                acc[1] += (p >> 8) & 0xFF;
                acc[2] += (p >> 16) & 0xFF;
            }
        }
        uint32_t* dstRow = dst + static_cast<size_t>(ty) * request.texW;
        for (int tx = 0; tx < request.texW; ++tx) {
            const uint32_t* acc = &sums[static_cast<size_t>(tx) * 3];
            dstRow[tx] =
                (255u << 24) | ((acc[2] / div) << 16) | ((acc[1] / div) << 8) | (acc[0] / div);
        }
    }
}

MemoryMapView::RenderRequest MemoryMapView::BuildRenderRequest(int texW, int texH, int gridW,
                                                               int gridH, int downscale) const
{
    RenderRequest request;
    request.texW = texW;
    request.texH = texH;
    request.gridW = gridW;
    request.gridH = gridH;
    request.downscale = downscale;
    request.startAddress = m_startAddress;
    request.colW = m_width;
    request.regionBytes = std::make_shared<std::vector<std::vector<uint8_t>>>();

    const int colStride = m_width + 1;
    const int numCols = std::max(1, (gridW + colStride - 1) / colStride);
    const uint64_t bytesPerCol = static_cast<uint64_t>(m_width / 8) * gridH;
    const uint64_t copyBegin = m_startAddress;
    const uint64_t copyEnd =
        std::min<uint64_t>(uint64_t{1} << 32, copyBegin + bytesPerCol * numCols);

    request.regions.reserve(m_regions.size());
    request.regionBytes->reserve(m_regions.size());
    for (const auto& source : m_regions) {
        const uint64_t regionBegin = source.amigaAddr;
        const uint64_t regionEnd = regionBegin + source.size;
        const uint64_t clippedBegin = std::max(regionBegin, copyBegin);
        const uint64_t clippedEnd = std::min(regionEnd, copyEnd);
        if (clippedBegin >= clippedEnd)
            continue;

        request.regionBytes->emplace_back();
        auto& bytes = request.regionBytes->back();
        if (source.hostPtr) {
            const size_t offset = static_cast<size_t>(clippedBegin - regionBegin);
            const size_t size = static_cast<size_t>(clippedEnd - clippedBegin);
            bytes.assign(source.hostPtr + offset, source.hostPtr + offset + size);
        }

        auto clipped = source;
        clipped.amigaAddr = static_cast<uint32_t>(clippedBegin);
        clipped.size = static_cast<uint32_t>(clippedEnd - clippedBegin);
        clipped.hostPtr = bytes.empty() ? nullptr : bytes.data();
        request.regions.push_back(std::move(clipped));
    }
    return request;
}

void MemoryMapView::RenderWorkerMain()
{
    while (true) {
        RenderRequest request;
        {
            std::unique_lock lock(m_renderMutex);
            m_renderCv.wait(lock, [this] { return m_stopThread || m_renderPending; });
            if (m_stopThread)
                return;
            request = std::move(m_renderRequest);
            m_renderPending = false;
        }

        std::vector<uint8_t> pixels;
        RenderRequestPixels(request, pixels);

        {
            std::lock_guard lock(m_renderMutex);
            if (m_stopThread)
                return;
            m_backPixels = std::move(pixels);
            m_readyTexW = request.texW;
            m_readyTexH = request.texH;
            m_pixelReady = true;
        }
    }
}

void MemoryMapView::BuildSegmentLayout(float availPx)
{
    m_segments.clear();

    // Pass 1: ordered segments in address space. Sort indices, never reorder m_regions (the
    // right-panel buttons + GetMemRegionColor key off the original index).
    std::vector<int> order(m_regions.size());
    for (int i = 0; i < (int)m_regions.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return m_regions[a].amigaAddr < m_regions[b].amigaAddr; });

    uint32_t cursor = 0;
    for (int idx : order) {
        const auto& r = m_regions[idx];
        uint32_t start = r.amigaAddr;
        uint32_t end = r.amigaAddr + r.size;
        if (end <= cursor)
            continue; // fully contained in an earlier region
        if (start < cursor)
            start = cursor; // overlap: clip to the uncovered part
        if (start > cursor)
            m_segments.push_back({true, -1, cursor, start, 0, 0, 0, 0}); // void gap
        m_segments.push_back({false, idx, start, end, 0, 0, 0, 0});
        cursor = end;
    }

    // Pass 2: assign pixel heights for availPx. Voids fixed; regions proportional + clamped.
    int numVoids = 0;
    uint64_t populatedTotal = 0;
    for (const auto& s : m_segments) {
        if (s.isVoid)
            ++numVoids;
        else
            populatedTotal += (s.addrEnd - s.addrStart);
    }
    float regionsAvail = availPx - numVoids * kVoidSegPx;

    float y = 0.0f;
    for (auto& s : m_segments) {
        float h;
        if (s.isVoid) {
            h = kVoidSegPx;
        } else if (populatedTotal > 0 && regionsAvail > 0.0f) {
            double frac = double(s.addrEnd - s.addrStart) / double(populatedTotal);
            h = std::clamp(static_cast<float>(regionsAvail * frac), kRegionMinPx, kRegionMaxPx);
        } else {
            h = kRegionMinPx;
        }
        s.y0 = y;
        s.y1 = y + h;
        y += h;
    }

    // Normalize so the stack fills availPx exactly (no scroll). Keeps the addr<->Y mapping inside
    // [0, availPx]; voids stay small while regions absorb the slack.
    if (y > 0.0f && availPx > 0.0f) {
        float scale = availPx / y;
        for (auto& s : m_segments) {
            s.y0 *= scale;
            s.y1 *= scale;
        }
    }

    // Map each segment's pixel span to contiguous texture rows summing to m_previewTexH.
    int prevRow = 0;
    float totalH = m_segments.empty() ? 1.0f : m_segments.back().y1;
    if (totalH <= 0.0f)
        totalH = 1.0f;
    for (size_t i = 0; i < m_segments.size(); ++i) {
        auto& s = m_segments[i];
        int endRow = (i + 1 == m_segments.size())
                         ? m_previewTexH
                         : static_cast<int>(s.y1 / totalH * m_previewTexH + 0.5f);
        endRow = std::clamp(endRow, prevRow, m_previewTexH);
        s.texRow0 = prevRow;
        s.texRowCount = endRow - prevRow;
        prevRow = endRow;
    }
}

void MemoryMapView::RenderPreview()
{
    uint32_t* pxPtr = reinterpret_cast<uint32_t*>(m_previewPixels.data());
    const uint32_t kVoidColor = (255u << 24) | (20u << 16) | (20u << 8) | 120u;

    // Default-fill (covers any rows not owned by a segment, e.g. empty regions).
    for (int i = 0; i < kPreviewW * m_previewTexH; ++i)
        pxPtr[i] = kVoidColor;

    for (const auto& s : m_segments) {
        if (s.texRowCount <= 0)
            continue;
        if (s.isVoid) {
            for (int row = s.texRow0; row < s.texRow0 + s.texRowCount; ++row)
                for (int x = 0; x < kPreviewW; ++x)
                    pxPtr[row * kPreviewW + x] = kVoidColor;
            continue;
        }

        const auto& r = m_regions[s.regionIdx];
        ImU32 c = GetMemRegionColor(s.regionIdx);
        uint8_t cr = static_cast<uint8_t>((c >> 0) & 0xFF);
        uint8_t cg = static_cast<uint8_t>((c >> 8) & 0xFF);
        uint8_t cb = static_cast<uint8_t>((c >> 16) & 0xFF);
        uint32_t segSpan = (s.addrEnd > s.addrStart) ? (s.addrEnd - s.addrStart) : 1;

        for (int local = 0; local < s.texRowCount; ++local) {
            int row = s.texRow0 + local;
            float brightness;
            if (r.hostPtr) {
                uint32_t addr =
                    s.addrStart + static_cast<uint32_t>(static_cast<uint64_t>(local) * segSpan /
                                                        static_cast<uint64_t>(s.texRowCount));
                uint8_t byteVal = r.hostPtr[addr - r.amigaAddr];
                brightness = byteVal / 255.0f * 0.55f + 0.25f; // 0.25 – 0.80 range
            } else {
                brightness = 0.30f; // mapped but no host data — flat low tint
            }
            uint32_t px = (255u << 24) | (static_cast<uint32_t>(cb * brightness) << 16) |
                          (static_cast<uint32_t>(cg * brightness) << 8) |
                          static_cast<uint32_t>(cr * brightness);
            for (int x = 0; x < kPreviewW; ++x)
                pxPtr[row * kPreviewW + x] = px;
        }
    }
}

bool MemoryMapView::PreviewAddrToY(uint32_t addr, float& outY) const
{
    for (const auto& s : m_segments) {
        if (addr >= s.addrStart && addr < s.addrEnd) {
            uint32_t span = (s.addrEnd > s.addrStart) ? (s.addrEnd - s.addrStart) : 1;
            outY = s.y0 + (addr - s.addrStart) / static_cast<float>(span) * (s.y1 - s.y0);
            return true;
        }
    }
    return false;
}

uint32_t MemoryMapView::PreviewYToAddr(float y) const
{
    if (m_segments.empty())
        return 0;
    y = std::clamp(y, m_segments.front().y0, m_segments.back().y1);
    for (const auto& s : m_segments) {
        if (y >= s.y0 && y <= s.y1) {
            if (s.isVoid)
                return s.addrStart;
            float h = (s.y1 > s.y0) ? (s.y1 - s.y0) : 1.0f;
            return s.addrStart + static_cast<uint32_t>((y - s.y0) / h * (s.addrEnd - s.addrStart));
        }
    }
    return m_segments.back().addrStart;
}

void MemoryMapView::UploadTexture(GLuint id, const uint8_t* px, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void MemoryMapView::EnsureMainTexture(int w, int h)
{
    if (w == m_mainTexW && h == m_mainTexH && m_mainTexId != 0)
        return;

    if (m_mainTexId != 0) {
        m_textureManager->ReleaseTexture(m_mainTexId);
        m_mainTexId = 0;
    }

    m_mainTexW = w;
    m_mainTexH = h;
    m_mainPixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    m_mainTexId = m_textureManager->CreateTexture(m_mainPixels.data(), w, h);
}

// Zoomed out, the main texture holds averaged blocks, which is no use for reading bits. Render the
// real memory pixels around the hovered grid pixel into a small texture and magnify that instead.
void MemoryMapView::DrawZoomedOutLoupe(int gx, int gy)
{
    const int rowBegin = std::clamp(gy - kLoupeH / 2, 0, std::max(0, m_gridH - kLoupeH));
    const int rowCount = std::min(kLoupeH, m_gridH);
    const int colBegin = std::clamp(gx - kLoupeW / 2, 0, std::max(0, m_gridW - kLoupeW));
    const int colCount = std::min(kLoupeW, m_gridW);
    if (rowCount <= 0 || colCount <= 0)
        return;

    m_loupeScratch.resize(static_cast<size_t>(m_gridW) * rowCount * 4);
    DoRenderPixels(m_loupeScratch.data(), m_gridW, m_gridH, rowBegin, rowCount, m_startAddress,
                   m_width, m_regions);

    m_loupePixels.assign(static_cast<size_t>(kLoupeW) * kLoupeH * 4, 0);
    for (int r = 0; r < rowCount; ++r)
        std::memcpy(&m_loupePixels[static_cast<size_t>(r) * kLoupeW * 4],
                    &m_loupeScratch[(static_cast<size_t>(r) * m_gridW + colBegin) * 4],
                    static_cast<size_t>(colCount) * 4);

    if (m_loupeTexId == 0)
        m_loupeTexId = m_textureManager->CreateTexture(m_loupePixels.data(), kLoupeW, kLoupeH);
    else
        UploadTexture(m_loupeTexId, m_loupePixels.data(), kLoupeW, kLoupeH);

    MagnifierStyle style;
    style.texelScreenW = 10.0f;
    style.texelScreenH = 10.0f;
    DrawTextureLoupe(m_loupeTexId, kLoupeW, kLoupeH, gx - colBegin, gy - rowBegin, kLoupeW, kLoupeH,
                     1, 1, style);
}

// ── Guess logic ───────────────────────────────────────────────────────────────

// Compute width in pixels from bplcon0/ddfstrt/ddfstop snapshot values. Resolution-aware
// (including AGA super-hires) via the shared DDF formula.
static int ComputeWidthPx(uint16_t bplcon0, uint16_t ddfstrt, uint16_t ddfstop)
{
    const core::BitplaneResolution res = core::ResolutionFromBplcon0(bplcon0);
    int fetchWords = core::FetchWordsPerRow(ddfstrt, ddfstop, res);
    if (fetchWords <= 0)
        fetchWords = 20 * core::ResolutionPixelsPerLores(res); // 20 / 40 / 80 words
    return fetchWords * 16;
}

// Extract active bitplane count from bplcon0, including the AGA 8th-plane bit (BPU3). Clamping
// this to 6 hid the 7th/8th plane of AGA screens from the guess table.
static int BpuFromBplcon0(uint16_t bplcon0)
{
    int bpu = (bplcon0 >> 12) & 0x7;
    if (bplcon0 & 0x0010)
        bpu |= 0x8;
    return std::min(bpu, core::BitplaneLayout::kMaxPlanes);
}

void MemoryMapView::ComputeGuesses()
{
    m_guesses.clear();

    if (!m_emulatorView->IsRunning())
        return;

    // --- Primary path: use per-frame bplpt event cache ---
    // Member scratch (was a function-local static shared across every instance of this view).
    if (m_bplptScratch.empty())
        m_bplptScratch.resize(adm::AmigaEmulator::kMaxBplptEvents);
    adm::AmigaEmulator::BplptEvent* events = m_bplptScratch.data();
    int eventCount = m_emulatorView->GetBplptEvents(events, adm::AmigaEmulator::kMaxBplptEvents);

    if (eventCount > 0) {
        // For each plane, collect all (vpos, ptr, width, bpu) entries across the frame.
        // We group by plane so we can estimate scanline height from consecutive writes.
        struct PlaneEvent
        {
            int vpos;
            uint32_t ptr;
            int width;
            int bpu; // active plane count from bplcon0 at time of write
        };
        std::vector<PlaneEvent> byPlane[8]; // index = plane 0-7

        for (int i = 0; i < eventCount; i++) {
            const auto& ev = events[i];
            if (ev.plane >= 8)
                continue;
            if (ev.ptr == 0)
                continue;
            int bpu = BpuFromBplcon0(ev.bplcon0);
            int w = ComputeWidthPx(ev.bplcon0, ev.ddfstrt, ev.ddfstop);
            byPlane[ev.plane].push_back({static_cast<int>(ev.vpos), ev.ptr, w, bpu});
        }

        // Build GuessEntry list. For each plane, each write gives a "region".
        // Height is estimated as the distance to the next write of the same plane,
        // or (256 - scanlineStart) for the last write in the frame.
        // A plane is "enabled" if bpu > plane_index at the time its pointer was written.
        static constexpr int kFrameLines = 256; // approximate visible PAL lines

        for (int p = 0; p < 8; p++) {
            auto& evs = byPlane[p];
            if (evs.empty())
                continue;
            for (int j = 0; j < static_cast<int>(evs.size()); j++) {
                // Skip duplicates (same ptr and same scanline as previous entry)
                if (j > 0 && evs[j].ptr == evs[j - 1].ptr && evs[j].vpos == evs[j - 1].vpos)
                    continue;

                int nextVpos =
                    (j + 1 < static_cast<int>(evs.size())) ? evs[j + 1].vpos : kFrameLines;
                int scanStart = evs[j].vpos;
                int scanEnd = nextVpos;
                int height = scanEnd - scanStart;
                if (height <= 0)
                    height = kFrameLines - scanStart;

                bool enabled = (evs[j].bpu > p);

                m_guesses.push_back({p + 1, evs[j].ptr & 0x1FFFFFu, evs[j].width, height, scanStart,
                                     scanEnd, enabled});
            }
        }
        return;
    }

    // --- Fallback: read end-of-frame register snapshot ---
    // Used when the new ADM event API is not available (older dylib).
    adm::AmigaEmulator* emu = m_emulatorView->GetEmulator();
    if (!emu)
        return;

    core::Custom custom{};
    if (!emu->GetCustomRegisters(custom))
        return;

    bool lace = (custom.bplcon0 >> 2) & 1;
    int displayH = lace ? 512 : 256;
    int bpu = BpuFromBplcon0(custom.bplcon0);
    int widthPx = ComputeWidthPx(custom.bplcon0, custom.ddfstrt, custom.ddfstop);

    for (int i = 0; i < bpu; i++) {
        uint32_t ptr = core::SwapPtrWords(custom.bplpt[i]) & 0x1FFFFFu;
        if (ptr == 0)
            continue;
        m_guesses.push_back({i + 1, ptr, widthPx, displayH, 0, displayH, true});
    }
}

void MemoryMapView::JumpToGuess(const GuessEntry& g)
{
    m_startAddress = g.address;
    m_width = g.width;
    std::snprintf(m_addressBuf, sizeof(m_addressBuf), "%06X", m_startAddress);
}

void MemoryMapView::DrawGuessTable()
{
    ImGui::Spacing();
    ImGui::SeparatorText("Bitplane Pointers");

    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterV | ImGuiTableFlags_RowBg |
        kAdjustableColumns | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX;

    // 6 columns: LED indicator | Plane | Address | W | H | Scan
    if (ImGui::BeginTable("##guesses", 6, flags)) {
        ImGui::TableSetupColumn(
            "##led", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 16.0f);
        ImGui::TableSetupColumn("Plane", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("H", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Scan", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableHeadersRow();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float ledRadius = 5.0f;

        for (int i = 0; i < static_cast<int>(m_guesses.size()); i++) {
            const auto& g = m_guesses[i];
            ImGui::TableNextRow();

            // Column 0: LED indicator
            ImGui::TableSetColumnIndex(0);
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                float midY = p.y + ImGui::GetTextLineHeight() * 0.5f;
                ImVec2 center = ImVec2(p.x + ledRadius + 1.0f, midY);
                ImU32 fill = g.enabled ? IM_COL32(0, 220, 0, 255) : IM_COL32(0, 55, 0, 255);
                drawList->AddCircleFilled(center, ledRadius, fill);
                drawList->AddCircle(center, ledRadius, IM_COL32(80, 80, 80, 255), 0, 1.0f);
                ImGui::Dummy(ImVec2(ledRadius * 2.0f + 2.0f, ImGui::GetTextLineHeight()));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(g.enabled ? "Enabled (BPLCON0.BPU >= plane)"
                                                : "Pointer set but not enabled in BPLCON0");
                }
            }

            // Column 1: Plane label (selectable spans remaining columns)
            ImGui::TableSetColumnIndex(1);
            char label[32];
            std::snprintf(label, sizeof(label), "BPL%d##g%d_%d", g.plane, g.plane, i);
            bool selected = (m_startAddress == g.address && m_width == g.width);
            if (ImGui::Selectable(label, selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, 0))) {
                JumpToGuess(g);
            }

            ImGui::TableSetColumnIndex(2);
            if (m_addrCtx && m_addrCtx->valid()) {
                char suffix[24];
                std::snprintf(suffix, sizeof(suffix), "gmap%d", i);
                DrawAddressLink(suffix, g.address, "%06X", *m_addrCtx);
            } else {
                ImGui::Text("%06X", g.address);
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", g.width);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", g.height);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d-%d", g.scanlineStart, g.scanlineEnd);
        }
        ImGui::EndTable();
    }
}

// ── Main draw ─────────────────────────────────────────────────────────────────

void MemoryMapView::FocusContainingPanels() { ui::FocusContainingPanels("Memory Map"); }

void MemoryMapView::Draw()
{
    if (!m_isOpen)
        return;

    if (m_focusFramesLeft > 0) {
        ImGui::SetNextWindowFocus();
        FocusContainingPanels();
        m_focusFramesLeft--;
    }

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    if (!ImGui::Begin(ICON_FA_MAP " Memory Map###Memory Map", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    if (!m_emulatorView->IsRunning()) {
        ImGui::TextDisabled("No emulator running.");
        ImGui::End();
        return;
    }

    // The overview data — memory regions, bitplane guesses, and the preview bitmap — only needs
    // rebuilding when the emulator actually advanced a frame. While paused (the common debugging
    // state) the memory contents are frozen, so we skip the region fetch, guess recompute, the
    // 128x400 preview scan, and its texture upload entirely.
    const uint64_t emuFrame = m_emulatorView->GetEmulatorFrameCount();
    const bool overviewStale = (emuFrame != m_lastPreviewFrame) || m_regions.empty();
    if (overviewStale) {
        m_regions = m_emulatorView->GetFreshMemoryRegions();
        ComputeGuesses();
    }
    m_lastPreviewFrame = emuFrame;

    // A ShowAddress() navigation requested adopting the row width from the bitplane guess whose
    // pointer matches the target address. Guesses are now computed, so resolve it here.
    if (m_pendingWidthFromGuess) {
        for (const auto& g : m_guesses) {
            if (g.address == m_startAddress) {
                m_width = g.width;
                break;
            }
        }
        m_pendingWidthFromGuess = false;
    }

    // ── Layout constants ──────────────────────────────────────────────────────
    static constexpr float kSplitterW = 6.0f;
    static constexpr float kMinLeftW = 80.0f;
    static constexpr float kMinRightW = 200.0f;
    const ImVec2 contentAvail = ImGui::GetContentRegionAvail();
    if (contentAvail.x <= 0.0f || contentAvail.y <= 0.0f) {
        ImGui::End();
        return;
    }
    m_leftColumnW = std::clamp(m_leftColumnW, kMinLeftW,
                               std::max(kMinLeftW, contentAvail.x - kSplitterW - kMinRightW));

    // ── Left child: preview (top) + horizontal splitter + bitplane table (bottom) ──────────
    ImGui::BeginChild("##mmLeft", ImVec2(m_leftColumnW, 0), false);

    // The bitplane table must always show its header + table-header row + at least 4 data rows.
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float tableMinH = ImGui::GetTextLineHeightWithSpacing() + // Spacing + SeparatorText
                            ImGui::GetFrameHeight() +               // table header row
                            4.0f * rowH +                           // 4 data rows
                            ImGui::GetStyle().ItemSpacing.y * 2.0f;
    const float leftAvailY = ImGui::GetContentRegionAvail().y;
    m_leftPreviewH = std::clamp(m_leftPreviewH, kPreviewMinH,
                                std::max(kPreviewMinH, leftAvailY - kLeftSplitterH - tableMinH));

    // ── Preview sub-area ───────────────────────────────────────────────────────
    ImGui::BeginChild("##mmPreview", ImVec2(0, m_leftPreviewH), false);
    {
        float dispW = ImGui::GetContentRegionAvail().x;
        float dispH = ImGui::GetContentRegionAvail().y;
        int newTexH = std::max(1, static_cast<int>(dispH));

        if (m_regions.empty()) {
            ImGui::TextDisabled("No memory regions");
        } else {
            // Resize/recreate the preview texture when the allocated height changes (a GL texture
            // can't grow in place), and flag the segment layout for a rebuild.
            if (newTexH != m_previewTexH) {
                m_previewTexH = newTexH;
                m_previewPixels.assign(kPreviewW * m_previewTexH * 4, 0);
                if (m_previewTexId != 0) {
                    m_textureManager->ReleaseTexture(m_previewTexId);
                    m_previewTexId = 0;
                }
                m_segLayoutDirty = true;
            }
            if (overviewStale)
                m_segLayoutDirty = true;

            if (m_segLayoutDirty) {
                BuildSegmentLayout(dispH);
                RenderPreview();
                if (m_previewTexId == 0)
                    m_previewTexId = m_textureManager->CreateTexture(m_previewPixels.data(),
                                                                     kPreviewW, m_previewTexH);
                else
                    UploadTexture(m_previewTexId, m_previewPixels.data(), kPreviewW, m_previewTexH);
                m_segLayoutDirty = false;
                m_lastPreviewDispH = newTexH;
            }

            ImVec2 previewSize(dispW, dispH);
            ImVec2 imgMin = ImGui::GetCursorScreenPos();
            ImVec2 imgMax(imgMin.x + previewSize.x, imgMin.y + previewSize.y);

            ImGui::InvisibleButton("##preview", previewSize, ImGuiButtonFlags_MouseButtonLeft);
            const bool previewActive = ImGui::IsItemActive();
            const bool previewHovered = ImGui::IsItemHovered();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (m_previewTexId != 0)
                dl->AddImage(TextureManager::ToImTextureID(m_previewTexId), imgMin, imgMax);

            if (previewHovered || previewActive)
                ImGui::SetMouseCursor(previewActive ? ImGuiMouseCursor_ResizeAll
                                                    : ImGuiMouseCursor_Hand);

            if (previewActive) {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
                    // Follow the cursor 1:1 in screen space through the piecewise map.
                    float curY;
                    if (PreviewAddrToY(m_startAddress, curY)) {
                        curY += ImGui::GetIO().MouseDelta.y;
                        m_startAddress = PreviewYToAddr(curY);
                        std::snprintf(m_addressBuf, sizeof(m_addressBuf), "%06X", m_startAddress);
                    }
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    float localY = ImGui::GetMousePos().y - imgMin.y;
                    m_startAddress = PreviewYToAddr(localY);
                    std::snprintf(m_addressBuf, sizeof(m_addressBuf), "%06X", m_startAddress);
                }
            }

            // Per-segment labels: region name + start addr, or a void notice + range.
            const float lineH = ImGui::GetTextLineHeight();
            for (const auto& s : m_segments) {
                if (s.y1 - s.y0 < lineH)
                    continue; // too short to label legibly
                ImVec2 tp(imgMin.x + 3.0f, imgMin.y + s.y0 + 1.0f);
                char buf[96];
                if (s.isVoid) {
                    std::snprintf(buf, sizeof(buf), "void %06X-%06X", s.addrStart, s.addrEnd);
                } else {
                    std::string nm = m_regions[s.regionIdx].name;
                    size_t paren = nm.find('(');
                    if (paren != std::string::npos) {
                        nm = nm.substr(0, paren);
                        size_t last = nm.find_last_not_of(' ');
                        if (last != std::string::npos)
                            nm = nm.substr(0, last + 1);
                    }
                    std::snprintf(buf, sizeof(buf), "%s  $%06X", nm.c_str(), s.addrStart);
                }
                dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), buf);
                dl->AddText(tp, IM_COL32(255, 255, 255, 230), buf);
            }

            // Sliding window overlay showing the currently-visible main view range.
            if (m_mainTexId != 0 && m_gridW > 0 && m_gridH > 0) {
                uint64_t mainBytes =
                    static_cast<uint64_t>(m_gridH) * static_cast<uint64_t>(m_width / 8);
                float yTop, yBot;
                if (!PreviewAddrToY(m_startAddress, yTop))
                    yTop = 0.0f;
                if (!PreviewAddrToY(static_cast<uint32_t>(m_startAddress + mainBytes), yBot))
                    yBot = dispH;
                yTop = std::clamp(yTop, 0.0f, dispH);
                yBot = std::clamp(yBot, 0.0f, dispH);
                dl->AddRect(ImVec2(imgMin.x, imgMin.y + yTop), ImVec2(imgMax.x, imgMin.y + yBot),
                            IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.5f);
            }
        }
    }
    ImGui::EndChild();

    // ── Horizontal splitter between preview and table ──────────────────────────
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        float sw = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##mmLeftSplitter", ImVec2(sw, kLeftSplitterH));
        if (ImGui::IsItemActive()) {
            m_leftPreviewH =
                std::clamp(m_leftPreviewH + ImGui::GetIO().MouseDelta.y, kPreviewMinH,
                           std::max(kPreviewMinH, leftAvailY - kLeftSplitterH - tableMinH));
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const float midY = sp.y + kLeftSplitterH * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x, midY), ImVec2(sp.x + sw, midY),
                                            IM_COL32(110, 110, 110, 180), 1.0f);
    }

    // ── Bitplane table sub-area ────────────────────────────────────────────────
    ImGui::BeginChild("##mmTable", ImVec2(0, 0), false);
    DrawGuessTable();
    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::SameLine();

    // ── Splitter ──────────────────────────────────────────────────────────────
    {
        const float splitterH = ImGui::GetContentRegionAvail().y;
        ImVec2 splitterPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##mmSplitter", ImVec2(kSplitterW, splitterH));
        if (ImGui::IsItemActive()) {
            m_leftColumnW =
                std::clamp(m_leftColumnW + ImGui::GetIO().MouseDelta.x, kMinLeftW,
                           std::max(kMinLeftW, contentAvail.x - kSplitterW - kMinRightW));
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const float midX = splitterPos.x + kSplitterW * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(midX, splitterPos.y),
                                            ImVec2(midX, splitterPos.y + splitterH),
                                            IM_COL32(110, 110, 110, 180), 1.0f);
    }

    ImGui::SameLine();

    // ── Right child: controls + main view ─────────────────────────────────────
    ImGui::BeginChild("##mmRight", ImVec2(0, 0), false);

    // Row 1: address + width inputs
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Addr");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputText("##addr", m_addressBuf, sizeof(m_addressBuf),
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase |
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_startAddress = static_cast<uint32_t>(std::strtoul(m_addressBuf, nullptr, 16));
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Width");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputInt("##width", &m_width, 16, 64)) {
        m_width = std::max(16, (m_width / 16) * 16);
    }

    // Pixel magnifier ("loupe") toggle. Highlighted while active; hover the map to zoom in.
    // Capture the toggle state before the Button so the conditional Pop matches the conditional
    // Push — the click below flips m_showMagnifier and would otherwise unbalance the style stack.
    ImGui::SameLine();
    const bool magnifierActive = m_showMagnifier;
    if (magnifierActive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS_PLUS "##mmMagnifier"))
        m_showMagnifier = !m_showMagnifier;
    if (magnifierActive)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(m_showMagnifier ? "Hide Magnifier" : "Magnifier (hover to zoom)");

    // Scale: zoom the whole map out to fit more memory on screen, or in for bigger pixels.
    ImGui::SameLine();
    ImGui::BeginDisabled(m_scaleStep <= kMinScaleStep);
    if (ImGui::Button(ICON_FA_MINUS "##mmScaleDown"))
        m_scaleStep = std::max(kMinScaleStep, m_scaleStep - 1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Scale down (Ctrl+Wheel)");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    {
        char scaleLabel[16];
        if (m_scaleStep < 0)
            std::snprintf(scaleLabel, sizeof(scaleLabel), "1/%dx", DownscaleFactor());
        else
            std::snprintf(scaleLabel, sizeof(scaleLabel), "%dx", UpscaleFactor());
        // Fixed width so the + button stays put as the label changes.
        const float labelW = ImGui::CalcTextSize("1/8x").x + ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::Button(scaleLabel, ImVec2(labelW, 0)))
            m_scaleStep = 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset scale");
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(m_scaleStep >= kMaxScaleStep);
    if (ImGui::Button(ICON_FA_PLUS "##mmScaleUp"))
        m_scaleStep = std::min(kMaxScaleStep, m_scaleStep + 1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Scale up (Ctrl+Wheel)");

    // Row 2: jump buttons (dynamic from regions)
    auto DrawRegionButton = [](const char* label, int regionIdx) -> bool {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float textH = ImGui::GetTextLineHeight();
        float swatchSz = textH * 0.72f;

        char buf[64];
        std::snprintf(buf, sizeof(buf), "    %s", label);
        bool clicked = ImGui::Button(buf);

        if (regionIdx >= 0) {
            ImU32 color = GetMemRegionColor(regionIdx);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pad = ImGui::GetStyle().FramePadding;
            ImVec2 pMin = ImVec2(pos.x + pad.x, pos.y + pad.y + (textH - swatchSz) * 0.5f);
            dl->AddRectFilled(pMin, ImVec2(pMin.x + swatchSz, pMin.y + swatchSz), color, 1.0f);
        }
        return clicked;
    };

    for (int i = 0; i < (int)m_regions.size(); ++i) {
        const auto& r = m_regions[i];
        if (i > 0)
            ImGui::SameLine();

        std::string label = r.name;
        size_t paren = label.find('(');
        if (paren != std::string::npos) {
            label = label.substr(0, paren);
            size_t last = label.find_last_not_of(' ');
            if (last != std::string::npos)
                label = label.substr(0, last + 1);
        }

        if (DrawRegionButton(label.c_str(), i)) {
            m_startAddress = r.amigaAddr;
            std::snprintf(m_addressBuf, sizeof(m_addressBuf), "%06X", m_startAddress);
        }
    }

    ImGui::Separator();

    // Main texture — fills remaining space in the right child. Zoomed out, the texture stays at
    // screen size and the memory grid behind it is `down` times larger; zoomed in, the grid (and
    // texture) is `up` times smaller and drawn stretched.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const int down = DownscaleFactor();
    const int up = UpscaleFactor();
    int texW = std::max(16, static_cast<int>(avail.x) / up);
    int texH = std::max(1, static_cast<int>(avail.y) / up);
    m_gridW = texW * down;
    m_gridH = texH * down;
    const float screenPerGrid = ScreenPerGridPx();

    EnsureMainTexture(texW, texH);

    if (m_mainTexId != 0 && m_mainTexW > 0 && m_mainTexH > 0) {
        // If the worker finished a frame that matches the current texture size, take the
        // completed buffer under the short publication lock and upload it on the UI thread.
        bool uploadReady = false;
        {
            std::lock_guard lock(m_renderMutex);
            if (m_pixelReady) {
                if (m_readyTexW == m_mainTexW && m_readyTexH == m_mainTexH) {
                    std::swap(m_mainPixels, m_backPixels);
                    uploadReady = true;
                }
                m_pixelReady = false;
            }
        }
        if (uploadReady)
            UploadTexture(m_mainTexId, m_mainPixels.data(), m_mainTexW, m_mainTexH);

        // Re-render the main view only when one of its inputs changed: the emulator advanced a
        // frame, the user panned (start address) or changed width, or the texture was resized.
        // This stops a render thread from spinning every UI frame while idle/paused.
        const bool mainInputsChanged =
            !m_mainKickValid || (emuFrame != m_lastMainKickFrame) ||
            (m_startAddress != m_lastMainKickAddr) || (m_width != m_lastMainKickWidth) ||
            (texW != m_lastMainKickTexW) || (texH != m_lastMainKickTexH) ||
            (m_scaleStep != m_lastMainKickScale);

        // Publish the latest request to the persistent worker. If it is still rendering an older
        // frame, this replaces any queued request so it catches up to the newest exact snapshot.
        if (mainInputsChanged) {
            m_mainKickValid = true;
            m_lastMainKickFrame = emuFrame;
            m_lastMainKickAddr = m_startAddress;
            m_lastMainKickWidth = m_width;
            m_lastMainKickTexW = texW;
            m_lastMainKickTexH = texH;
            m_lastMainKickScale = m_scaleStep;

            RenderRequest request = BuildRenderRequest(texW, texH, m_gridW, m_gridH, down);
            {
                std::lock_guard lock(m_renderMutex);
                m_renderRequest = std::move(request);
                m_renderPending = true;
            }
            m_renderCv.notify_one();
        }

        ImVec2 imgPos = ImGui::GetCursorScreenPos();
        ImVec2 imgSize(static_cast<float>(m_mainTexW * up), static_cast<float>(m_mainTexH * up));
        ImGui::InvisibleButton("##mainmap", imgSize, ImGuiButtonFlags_MouseButtonLeft);
        const bool isActive = ImGui::IsItemActive();

        if (ImGui::IsItemHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f) {
            m_scaleStep = std::clamp(m_scaleStep + (ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1),
                                     kMinScaleStep, kMaxScaleStep);
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
        }

        ImGui::GetWindowDrawList()->AddImage(TextureManager::ToImTextureID(m_mainTexId), imgPos,
                                             ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));

        // Blit highlight: tint the destination (green) and any active source channels
        // (A=cyan, B=magenta, C=orange) wherever they fall within the currently-shown memory.
        //
        // The tint is translucent so the memory content underneath stays readable — the point
        // of the highlight is to say "this blit touched *these* bytes", which is useless if it
        // paints over the bytes. A highlight row is only one pixel tall, so an outline is not
        // an option; alpha is.
        //
        // Channels are drawn A, B, C then D, so where they overlap the colours blend rather
        // than the last one winning outright. That is worth having: a cookie-cut blit points C
        // and D at the same address, and the blended band shows the read-modify-write region.
        if (m_activeBlit) {
            core::MemoryGridLayout g;
            g.startAddress = m_startAddress;
            g.colW = m_width;
            g.texW = m_gridW;
            g.texH = m_gridH;

            // Enough tint to identify the channel at a glance, little enough that the memory
            // pixels beneath still read. The destination gets a touch more weight than the
            // read channels because it is the one most often being looked for.
            constexpr int kDestAlpha = 130;
            constexpr int kSourceAlpha = 100;

            auto colorFor = [](core::BlitMemChannel ch) -> ImU32 {
                switch (ch) {
                    case core::BlitMemChannel::Dest:
                        return IM_COL32(90, 255, 120, kDestAlpha);
                    case core::BlitMemChannel::SourceA:
                        return IM_COL32(80, 220, 255, kSourceAlpha);
                    case core::BlitMemChannel::SourceB:
                        return IM_COL32(255, 120, 230, kSourceAlpha);
                    case core::BlitMemChannel::SourceC:
                        return IM_COL32(255, 180, 70, kSourceAlpha);
                }
                return IM_COL32(255, 255, 255, kSourceAlpha);
            };

            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (const auto& r : core::MapBlitToMemoryGrid(*m_activeBlit, g)) {
                ImVec2 tl(imgPos.x + static_cast<float>(r.x0) * screenPerGrid,
                          imgPos.y + static_cast<float>(r.y0) * screenPerGrid);
                ImVec2 br(imgPos.x + static_cast<float>(r.x1) * screenPerGrid,
                          imgPos.y + static_cast<float>(r.y1) * screenPerGrid);
                // Keep a one-row run visible when zoomed out below a screen pixel.
                br.x = std::max(br.x, tl.x + 1.0f);
                br.y = std::max(br.y, tl.y + 1.0f);
                dl->AddRectFilled(tl, br, colorFor(r.channel));
            }
        }

        if (ImGui::IsItemHovered() || isActive)
            ImGui::SetMouseCursor(isActive ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);

        if (isActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            // Pan in memory pixels, so the map tracks the mouse at any scale.
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            delta.x /= screenPerGrid;
            delta.y /= screenPerGrid;
            int bytesPerRow = std::max(1, m_width / 8);

            m_dragAccumY -= delta.y;
            m_dragAccumX -= delta.x;

            if (std::abs(m_dragAccumY) >= 1.0f) {
                int rows = static_cast<int>(m_dragAccumY);
                m_dragAccumY -= static_cast<float>(rows);
                int64_t newAddr = static_cast<int64_t>(m_startAddress) +
                                  static_cast<int64_t>(rows) * static_cast<int64_t>(bytesPerRow);
                m_startAddress =
                    static_cast<uint32_t>(std::clamp(newAddr, int64_t{0}, int64_t{0xFFFFFF}));
            }

            if (std::abs(m_dragAccumX) >= 16.0f) {
                int words = static_cast<int>(m_dragAccumX / 16.0f);
                m_dragAccumX -= static_cast<float>(words) * 16.0f;
                int64_t newAddr =
                    static_cast<int64_t>(m_startAddress) + static_cast<int64_t>(words) * 2;
                m_startAddress =
                    static_cast<uint32_t>(std::clamp(newAddr, int64_t{0}, int64_t{0xFFFFFF}));
            }

            std::snprintf(m_addressBuf, sizeof(m_addressBuf), "%06X", m_startAddress);
        }

        // Pixel magnifier: zoom into the memory bitmap under the mouse (idle hover only, so it
        // doesn't fight the drag-to-pan gesture).
        if (m_showMagnifier && !isActive && ImGui::IsItemHovered()) {
            ImVec2 mouse = ImGui::GetMousePos();
            int texX =
                std::clamp(static_cast<int>((mouse.x - imgPos.x) / screenPerGrid), 0, m_gridW - 1);
            int texY =
                std::clamp(static_cast<int>((mouse.y - imgPos.y) / screenPerGrid), 0, m_gridH - 1);

            // Resolve the chip-RAM address under the cursor. The bitmap is laid out in vertical
            // columns of (m_width)+1 px (the +1 is the inter-column separator).
            const int bytesPerRow = std::max(1, m_width / 8);
            const int colStride = m_width + 1;
            const int col = texX / colStride;
            const int xInCol = texX % colStride;
            uint32_t hoverAddr = m_startAddress +
                                 static_cast<uint32_t>(col) * static_cast<uint32_t>(bytesPerRow) *
                                     static_cast<uint32_t>(m_gridH) +
                                 static_cast<uint32_t>(texY) * static_cast<uint32_t>(bytesPerRow) +
                                 static_cast<uint32_t>(std::min(xInCol, m_width - 1) / 8);

            const float kZoom = 10.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(32.0f * kZoom, 0),
                                                ImVec2(32.0f * kZoom, FLT_MAX));
            ImGui::BeginTooltip();
            MagnifierStyle style;
            style.texelScreenW = kZoom;
            style.texelScreenH = kZoom;
            if (down > 1)
                DrawZoomedOutLoupe(texX, texY);
            else
                DrawTextureLoupe(m_mainTexId, m_mainTexW, m_mainTexH, texX, texY, kLoupeW, kLoupeH,
                                 1, 1, style);
            ImGui::Separator();
            if (xInCol >= m_width)
                ImGui::TextUnformatted("(column separator)");
            else
                ImGui::Text("Addr: %06X", hoverAddr & 0xFFFFFF);
            ImGui::EndTooltip();
        }
    }

    ImGui::EndChild();

    ImGui::End();
}

} // namespace adm::ui
