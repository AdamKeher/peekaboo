#pragma once

#include <algorithm>
#include <cstdint>

#include <glad/glad.h>
#include <imgui.h>

namespace adm::ui {

// Centralised "loupe" magnifier used by both the Amiga Emulator display and the Memory Map view.
//
// It magnifies a rectangular region of a GL texture, centred on a caller-chosen texel, and draws
// it nearest-neighbour into the current ImGui window/tooltip at the current cursor position. The
// caller owns all coordinate math (which is inherently domain-specific) and any surrounding chrome
// (BeginTooltip/EndTooltip, info labels); this helper only paints the zoomed pixels, an optional
// crosshair, and an optional highlight cell.
struct MagnifierStyle
{
    // On-screen size (pixels) of a single source texel. Separate X/Y so non-square texels (e.g. a
    // hi-res emulator framebuffer) still render as square logical pixels.
    float texelScreenW = 16.0f;
    float texelScreenH = 16.0f;

    bool crosshair = true;
    float crosshairThickness = 1.0f;
    ImU32 crosshairColor = IM_COL32(255, 255, 100, 160);
    ImU32 highlightColor = IM_COL32(255, 255, 255, 255);
};

// Magnify a `srcW x srcH` block of texels centred on (centerTexX, centerTexY) and draw it at the
// current cursor. A `hlW x hlH`-texel cell anchored at (centerTexX, centerTexY) is outlined to mark
// the "unit" under inspection (1 texel for the memory map, one lores pixel for the display).
inline void DrawTextureLoupe(GLuint tex, int texW, int texH, int centerTexX, int centerTexY,
                             int srcW, int srcH, int hlW, int hlH, const MagnifierStyle& style)
{
    if (tex == 0 || texW <= 0 || texH <= 0 || srcW <= 0 || srcH <= 0)
        return;

    // Clamp the source window so it stays inside the texture.
    int srcX = std::clamp(centerTexX - srcW / 2, 0, std::max(0, texW - srcW));
    int srcY = std::clamp(centerTexY - srcH / 2, 0, std::max(0, texH - srcH));

    float u0 = srcX / static_cast<float>(texW);
    float v0 = srcY / static_cast<float>(texH);
    float u1 = (srcX + srcW) / static_cast<float>(texW);
    float v1 = (srcY + srcH) / static_cast<float>(texH);

    float outW = srcW * style.texelScreenW;
    float outH = srcH * style.texelScreenH;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Nearest-neighbour sampling for crisp pixels while magnified.
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    ImGui::Image((ImTextureID)(uintptr_t)tex, ImVec2(outW, outH), ImVec2(u0, v0), ImVec2(u1, v1));
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

    // Local (loupe-space) top-left of the highlighted unit cell.
    float cellX = origin.x + (centerTexX - srcX) * style.texelScreenW;
    float cellY = origin.y + (centerTexY - srcY) * style.texelScreenH;
    float cellW = hlW * style.texelScreenW;
    float cellH = hlH * style.texelScreenH;

    if (style.crosshair) {
        float ccx = cellX + cellW * 0.5f;
        float ccy = cellY + cellH * 0.5f;
        dl->AddLine({origin.x, ccy}, {origin.x + outW, ccy}, style.crosshairColor,
                    style.crosshairThickness);
        dl->AddLine({ccx, origin.y}, {ccx, origin.y + outH}, style.crosshairColor,
                    style.crosshairThickness);
    }

    if (hlW > 0 && hlH > 0) {
        // Black backing then bright outline so the cell reads on any content.
        dl->AddRect({cellX - 1, cellY - 1}, {cellX + cellW + 1, cellY + cellH + 1},
                    IM_COL32(0, 0, 0, 220), 0.0f, 0, 1.5f);
        dl->AddRect({cellX, cellY}, {cellX + cellW, cellY + cellH}, style.highlightColor, 0.0f, 0,
                    2.0f);
    }
}

} // namespace adm::ui
