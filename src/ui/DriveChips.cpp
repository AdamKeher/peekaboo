#include "DriveChips.h"

#include <algorithm>
#include <cstdint>

namespace adm {
namespace ui {

namespace {

// Segment order: a top, b top-right, c bottom-right, d bottom, e bottom-left, f top-left,
// g middle - the conventional lettering.
//
//   aaaa
//  f    b
//  f    b
//   gggg
//  e    c
//  e    c
//   dddd
constexpr uint8_t kSegA = 1 << 0;
constexpr uint8_t kSegB = 1 << 1;
constexpr uint8_t kSegC = 1 << 2;
constexpr uint8_t kSegD = 1 << 3;
constexpr uint8_t kSegE = 1 << 4;
constexpr uint8_t kSegF = 1 << 5;
constexpr uint8_t kSegG = 1 << 6;

constexpr uint8_t kDigitSegments[10] = {
    /* 0 */ kSegA | kSegB | kSegC | kSegD | kSegE | kSegF,
    /* 1 */ kSegB | kSegC,
    /* 2 */ kSegA | kSegB | kSegG | kSegE | kSegD,
    /* 3 */ kSegA | kSegB | kSegG | kSegC | kSegD,
    /* 4 */ kSegF | kSegG | kSegB | kSegC,
    /* 5 */ kSegA | kSegF | kSegG | kSegC | kSegD,
    /* 6 */ kSegA | kSegF | kSegG | kSegE | kSegC | kSegD,
    /* 7 */ kSegA | kSegB | kSegC,
    /* 8 */ kSegA | kSegB | kSegC | kSegD | kSegE | kSegF | kSegG,
    /* 9 */ kSegA | kSegB | kSegC | kSegD | kSegF | kSegG,
};

// A digit is half as wide as it is tall, which is the usual seven-segment proportion.
float DigitWidth(float height) { return height * 0.5f; }
float DigitGap(float height) { return height * 0.14f; }

void DrawDigit(ImDrawList* dl, ImVec2 pos, float height, int digit, ImU32 on, ImU32 off)
{
    const float w = DigitWidth(height);
    const float t = std::max(1.0f, height * 0.13f); // segment thickness
    const float inset = t * 0.6f;                   // keeps corners from overlapping

    // `digit` < 0 draws only the middle bar lit, which is the "-" of a "--" readout.
    const uint8_t mask = (digit >= 0 && digit <= 9) ? kDigitSegments[digit] : kSegG;

    auto seg = [&](uint8_t bit, ImVec2 a, ImVec2 b) {
        dl->AddRectFilled(ImVec2(pos.x + a.x, pos.y + a.y), ImVec2(pos.x + b.x, pos.y + b.y),
                          (mask & bit) ? on : off);
    };

    const float midY = height * 0.5f;
    // Horizontals
    seg(kSegA, ImVec2(inset, 0.0f), ImVec2(w - inset, t));
    seg(kSegG, ImVec2(inset, midY - t * 0.5f), ImVec2(w - inset, midY + t * 0.5f));
    seg(kSegD, ImVec2(inset, height - t), ImVec2(w - inset, height));
    // Verticals
    seg(kSegF, ImVec2(0.0f, inset), ImVec2(t, midY - inset));
    seg(kSegB, ImVec2(w - t, inset), ImVec2(w, midY - inset));
    seg(kSegE, ImVec2(0.0f, midY + inset), ImVec2(t, height - inset));
    seg(kSegC, ImVec2(w - t, midY + inset), ImVec2(w, height - inset));
}

} // namespace

float SevenSegmentWidth(float height) { return DigitWidth(height) * 2.0f + DigitGap(height); }

void DrawSevenSegment(ImDrawList* dl, ImVec2 pos, float height, int value, ImU32 on, ImU32 off)
{
    const bool blank = (value < 0);
    const int tens = blank ? -1 : (value / 10) % 10;
    const int units = blank ? -1 : value % 10;

    DrawDigit(dl, pos, height, tens, on, off);
    DrawDigit(dl, ImVec2(pos.x + DigitWidth(height) + DigitGap(height), pos.y), height, units, on,
              off);
}

int DrawDriveChips(const char* id, const std::vector<DriveChip>& chips, int selected,
                   ChipIndicatorClick* outClick)
{
    if (outClick)
        *outClick = ChipIndicatorClick{};
    if (chips.empty())
        return -1;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const float pad = style.FramePadding.x * 1.5f;
    const float dotR = lineH * 0.22f;
    const float segH = lineH * 0.95f;
    const float glyphGap = lineH * 0.35f;

    const bool anySubtitle = std::any_of(chips.begin(), chips.end(),
                                         [](const DriveChip& c) { return !c.subtitle.empty(); });
    const bool anyDisplay = std::any_of(chips.begin(), chips.end(),
                                        [](const DriveChip& c) { return c.digits != kNoDisplay; });

    // Every chip is sized to the widest one, so the strip is a fixed grid: nothing moves when a
    // drive's state changes.
    float nameW = 0.0f;
    float subW = 0.0f;
    size_t maxIndicators = 0;
    bool anyBacking = false;
    for (const auto& c : chips) {
        nameW = std::max(nameW, ImGui::CalcTextSize(c.name ? c.name : "").x);
        if (!c.subtitle.empty())
            subW = std::max(subW, ImGui::CalcTextSize(c.subtitle.c_str()).x);
        maxIndicators = std::max(maxIndicators, c.indicators.size());
        anyBacking = anyBacking || (c.backingGlyph != nullptr);
    }

    // The indicator glyphs come from the icon font and are not all the same advance width, so
    // reserve the widest of the ones actually in use for every slot.
    float indicatorW = 0.0f;
    for (const auto& c : chips)
        for (const auto& ind : c.indicators)
            indicatorW = std::max(indicatorW, ImGui::CalcTextSize(ind.glyph).x);
    float backingW = 0.0f;
    if (anyBacking) {
        for (const auto& c : chips)
            if (c.backingGlyph)
                backingW = std::max(backingW, ImGui::CalcTextSize(c.backingGlyph).x);
    }

    float contentW = dotR * 2.0f + glyphGap + nameW;
    if (anyBacking)
        contentW += glyphGap + backingW;
    if (anyDisplay)
        contentW += glyphGap + SevenSegmentWidth(segH);
    if (maxIndicators > 0)
        contentW += static_cast<float>(maxIndicators) * (indicatorW + glyphGap);
    contentW = std::max(contentW, dotR * 2.0f + glyphGap + subW);

    const float chipW = contentW + pad * 2.0f;
    const float chipH = (anySubtitle ? lineH * 2.0f + style.ItemSpacing.y * 0.5f : lineH) +
                        style.FramePadding.y * 2.0f;

    // Where the indicator run starts, as an offset from the chip origin. The same arithmetic
    // the draw loop walks below, hoisted so a clickable indicator's hit rect can be submitted
    // before anything is drawn.
    float indicatorsX0 = pad + dotR * 2.0f + glyphGap + nameW + glyphGap;
    if (anyBacking)
        indicatorsX0 += backingW + glyphGap;
    if (anyDisplay)
        indicatorsX0 += SevenSegmentWidth(segH) + glyphGap;

    const ImU32 colOn = ImGui::GetColorU32(ImVec4(0.35f, 0.80f, 0.35f, 1.0f));
    const ImU32 colOff = ImGui::GetColorU32(ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
    const ImU32 colSegOff = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    const ImU32 colLedOff = ImGui::GetColorU32(ImVec4(0.85f, 0.30f, 0.30f, 1.0f));

    int clicked = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Wrap rather than run off the window edge: four chips at a readable size do not fit a
    // narrow window, and a chip the user cannot see is worse than a second row.
    const float gap = style.ItemSpacing.x * 0.5f;
    const float avail = ImGui::GetContentRegionAvail().x;
    int perRow = static_cast<int>((avail + gap) / (chipW + gap));
    perRow = std::max(1, std::min(perRow, static_cast<int>(chips.size())));

    ImGui::PushID(id);
    for (int i = 0; i < static_cast<int>(chips.size()); ++i) {
        const DriveChip& c = chips[i];
        ImGui::PushID(i);
        if (i > 0 && (i % perRow) != 0)
            ImGui::SameLine(0.0f, gap);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float rowTop = origin.y + style.FramePadding.y;

        // Clickable indicators get their own hit rects, submitted BEFORE the chip's own
        // button. ImGui hands HoveredId to the first item that claims the point and rejects
        // later overlapping ones, so the chip below sees itself as not hovered and a click on
        // the padlock does not also change the drive selection.
        int hoveredIndicator = -1;
        for (size_t k = 0; k < c.indicators.size(); ++k) {
            if (!c.indicators[k].clickable || indicatorW <= 0.0f)
                continue;
            ImGui::SetCursorScreenPos(ImVec2(
                origin.x + indicatorsX0 + static_cast<float>(k) * (indicatorW + glyphGap), rowTop));
            ImGui::PushID(static_cast<int>(k));
            ImGui::InvisibleButton("##ind", ImVec2(indicatorW, lineH));
            ImGui::PopID();
            if (ImGui::IsItemHovered())
                hoveredIndicator = static_cast<int>(k);
            if (ImGui::IsItemClicked() && outClick)
                *outClick = ChipIndicatorClick{i, static_cast<int>(k)};
        }
        ImGui::SetCursorScreenPos(origin);

        ImGui::InvisibleButton("##chip", ImVec2(chipW, chipH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && i != selected)
            clicked = i;

        // Frame. The selected chip is filled like an active tab; hover gets a lighter fill.
        // An indicator under the cursor counts as hovering the chip, so the fill does not
        // flicker off as the pointer crosses the padlock.
        const ImU32 fill =
            ImGui::GetColorU32(i == selected                        ? ImGuiCol_TabActive
                               : (hovered || hoveredIndicator >= 0) ? ImGuiCol_TabHovered
                                                                    : ImGuiCol_Tab);
        dl->AddRectFilled(origin, ImVec2(origin.x + chipW, origin.y + chipH), fill,
                          style.FrameRounding);
        dl->AddRect(origin, ImVec2(origin.x + chipW, origin.y + chipH),
                    ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);

        float x = origin.x + pad;
        const float rowY = rowTop;
        const float midY = rowY + lineH * 0.5f;

        // LED, left of the name.
        dl->AddCircleFilled(ImVec2(x + dotR, midY), dotR, c.ledOn ? colOn : colLedOff, 12);
        x += dotR * 2.0f + glyphGap;

        dl->AddText(ImVec2(x, rowY), ImGui::GetColorU32(ImGuiCol_Text), c.name ? c.name : "");
        x += nameW + glyphGap;

        if (anyBacking) {
            if (c.backingGlyph)
                dl->AddText(ImVec2(x, rowY), ImGui::GetColorU32(c.backingColor), c.backingGlyph);
            x += backingW + glyphGap;
        }

        if (anyDisplay) {
            if (c.digits != kNoDisplay) {
                DrawSevenSegment(dl, ImVec2(x, rowY + (lineH - segH) * 0.5f), segH, c.digits,
                                 ImGui::GetColorU32(c.digitsColor), colSegOff);
            }
            x += SevenSegmentWidth(segH) + glyphGap;
        }

        for (size_t k = 0; k < c.indicators.size(); ++k) {
            const ChipIndicator& ind = c.indicators[k];
            if (static_cast<int>(k) == hoveredIndicator) {
                // A faint plate so a clickable glyph reads as a button rather than a lamp.
                dl->AddRectFilled(ImVec2(x - glyphGap * 0.4f, rowY),
                                  ImVec2(x + indicatorW + glyphGap * 0.4f, rowY + lineH),
                                  ImGui::GetColorU32(ImGuiCol_ButtonHovered), style.FrameRounding);
            }
            dl->AddText(ImVec2(x, rowY), ind.lit ? ImGui::GetColorU32(ind.litColor) : colOff,
                        ind.glyph);
            x += indicatorW + glyphGap;
        }

        if (!c.subtitle.empty()) {
            dl->AddText(ImVec2(origin.x + pad, rowY + lineH + style.ItemSpacing.y * 0.5f),
                        ImGui::GetColorU32(c.subtitleColor), c.subtitle.c_str());
        }

        if (hoveredIndicator >= 0 && !c.indicators[hoveredIndicator].tooltip.empty())
            ImGui::SetTooltip("%s", c.indicators[hoveredIndicator].tooltip.c_str());
        else if ((hovered || hoveredIndicator >= 0) && !c.tooltip.empty())
            ImGui::SetTooltip("%s", c.tooltip.c_str());

        ImGui::PopID();
    }
    ImGui::PopID();

    return clicked;
}

} // namespace ui
} // namespace adm
