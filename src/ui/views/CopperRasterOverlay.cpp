#include "CopperRasterOverlay.h"

#include <glad/glad.h>

#include <algorithm>

#include "../../core/Constants.h"
#include "../DmaColors.h"

namespace adm::ui {

namespace copraster = adm::core::copraster;

namespace {

constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;

// Seeds the raster's channel colours from the app's shared DMA palette, so this view, the frame
// scrubber's DMA row and DmaStatsView cannot drift apart. IM_COL32 and copraster::PackRGBA use
// the same byte order, so the values pass straight through.
void SeedChannelColors(copraster::Palette& pal)
{
    for (int type = 0; type < 10; ++type)
        pal.channel[type] = static_cast<uint32_t>(DmaTypeColor(type));
    // Idle slots are left transparent rather than painted: the picture underneath is more useful
    // than a flat dark wash over the whole frame.
    pal.channel[0] = 0;
}

} // namespace

CopperRasterOverlay::~CopperRasterOverlay()
{
    // No GL context is guaranteed at teardown; Reset() is the supported path and the owner calls
    // it. Deleting the texture here as well would risk a call on a dead context.
    m_result.pixels.clear();
    m_result.cells.clear();
}

void CopperRasterOverlay::Reset()
{
    if (m_texId != 0) {
        glDeleteTextures(1, &m_texId);
        m_texId = 0;
    }
    m_texW = m_texH = 0;
    m_result = copraster::Result{};
    m_result.pixels.shrink_to_fit();
    m_result.cells.shrink_to_fit();
}

void CopperRasterOverlay::EnsureTexture(int w, int h)
{
    if (w == m_texW && h == m_texH && m_texId != 0)
        return;

    if (m_texId == 0)
        glGenTextures(1, &m_texId);

    m_texW = w;
    m_texH = h;

    glBindTexture(GL_TEXTURE_2D, m_texId);
    // Nearest, not linear: one texel is one colour clock, and interpolating would smear a slot
    // into its neighbours — exactly the distinction this view exists to make.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void CopperRasterOverlay::UploadTexture()
{
    if (m_texId == 0 || m_result.pixels.empty())
        return;
    glBindTexture(GL_TEXTURE_2D, m_texId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_texW, m_texH, GL_RGBA, GL_UNSIGNED_BYTE,
                    m_result.pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void CopperRasterOverlay::Update(const adm::core::RasterFrameMap& map, const copraster::Input& in)
{
    SeedChannelColors(m_palette);
    copraster::Build(in, map, m_palette, m_options, m_result);

    if (m_result.width <= 0 || m_result.height <= 0)
        return;

    EnsureTexture(m_result.width, m_result.height);
    UploadTexture();
}

void CopperRasterOverlay::Draw(ImDrawList* dl, ImVec2 rasterMin, ImVec2 rasterMax) const
{
    if (!dl || m_texId == 0 || m_texW <= 0 || m_texH <= 0 || m_opacity <= 0.0f)
        return;

    const int a = std::clamp(static_cast<int>(m_opacity * 255.0f), 0, 255);

    // The GL backend binds its own sampler objects (imgui_impl_opengl3, GL 3.3+), and the linear
    // one is the default for every draw command. That overrides the GL_NEAREST set on the texture
    // itself, so without this the raster is bilinearly smeared the moment the window is scaled up
    // -- the one thing EnsureTexture() is explicitly trying to avoid. EmulatorView brackets the
    // emulator image the same way.
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    dl->AddImage(static_cast<ImTextureID>(m_texId), rasterMin, rasterMax, ImVec2(0.0f, 0.0f),
                 ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, a));
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
}

void CopperRasterOverlay::CellRect(int cc, int line, ImVec2 rasterMin, ImVec2 rasterMax,
                                   ImVec2& outMin, ImVec2& outMax) const
{
    const float w = rasterMax.x - rasterMin.x;
    const float h = rasterMax.y - rasterMin.y;
    const float cw = (m_result.width > 0) ? w / static_cast<float>(m_result.width) : 0.0f;
    const float ch = (m_result.height > 0) ? h / static_cast<float>(m_result.height) : 0.0f;

    outMin = {rasterMin.x + static_cast<float>(cc) * cw,
              rasterMin.y + static_cast<float>(line) * ch};
    // A colour clock can be well under a pixel wide at small window sizes; widen the marker so a
    // selection is still findable rather than vanishing into a sub-pixel sliver.
    outMax = {outMin.x + std::max(cw, 2.0f), outMin.y + std::max(ch, 2.0f)};
}

void CopperRasterOverlay::DrawMarkers(ImDrawList* dl, ImVec2 rasterMin, ImVec2 rasterMax) const
{
    if (!dl || m_result.width <= 0 || m_result.height <= 0)
        return;

    auto markLRef = [&](uint32_t lRef, ImU32 col, float thickness) {
        if (lRef == UINT32_MAX)
            return;
        const int line = static_cast<int>(lRef / kCCKPerLine);
        const int cc = static_cast<int>(lRef % kCCKPerLine);
        if (line < 0 || line >= m_result.height || cc < 0 || cc >= m_result.width)
            return;
        ImVec2 mn, mx;
        CellRect(cc, line, rasterMin, rasterMax, mn, mx);
        dl->AddRect({mn.x - 1.0f, mn.y - 1.0f}, {mx.x + 1.0f, mx.y + 1.0f}, col, 0.0f, 0,
                    thickness);
    };

    // The scrubber playhead reads as a full scanline plus a marker on the exact clock, so the
    // beam position is findable at any zoom without hunting for a single cell.
    if (m_playheadLRef != UINT32_MAX) {
        const int line = static_cast<int>(m_playheadLRef / kCCKPerLine);
        if (line >= 0 && line < m_result.height) {
            ImVec2 mn, mx;
            CellRect(0, line, rasterMin, rasterMax, mn, mx);
            dl->AddLine({rasterMin.x, mn.y}, {rasterMax.x, mn.y}, IM_COL32(255, 255, 255, 90),
                        1.0f);
        }
        markLRef(m_playheadLRef, IM_COL32(255, 255, 255, 200), 1.5f);
    }

    markLRef(m_highlightedLRef, IM_COL32(255, 220, 80, 230), 2.0f);
    markLRef(m_selectedLRef, IM_COL32(255, 255, 255, 255), 2.0f);
}

const copraster::Cell* CopperRasterOverlay::CellAt(ImVec2 pos, ImVec2 rasterMin, ImVec2 rasterMax,
                                                   int& outCC, int& outLine) const
{
    outCC = -1;
    outLine = -1;
    if (m_result.width <= 0 || m_result.height <= 0)
        return nullptr;

    const float w = rasterMax.x - rasterMin.x;
    const float h = rasterMax.y - rasterMin.y;
    if (w <= 0.0f || h <= 0.0f)
        return nullptr;
    if (pos.x < rasterMin.x || pos.x >= rasterMax.x || pos.y < rasterMin.y || pos.y >= rasterMax.y)
        return nullptr;

    const int cc = static_cast<int>((pos.x - rasterMin.x) / w * static_cast<float>(m_result.width));
    const int line =
        static_cast<int>((pos.y - rasterMin.y) / h * static_cast<float>(m_result.height));
    if (cc < 0 || cc >= m_result.width || line < 0 || line >= m_result.height)
        return nullptr;

    // The columns past the end of the Agnus line are the next line's leading clocks (see
    // copraster::Build's wrap pass), so report them as such: a click there must select the clock
    // that is actually drawn under the cursor, not a phantom hpos 230.
    const int lineCC = (m_result.LineCCAt(line) > 0) ? m_result.LineCCAt(line) : m_result.width;
    if (cc >= lineCC && line + 1 < m_result.height) {
        outCC = cc - lineCC;
        outLine = line + 1;
        return m_result.At(outCC, outLine);
    }

    outCC = cc;
    outLine = line;
    return m_result.At(cc, line);
}

} // namespace adm::ui
