#include "BlitterMintermView.h"

#include "../IconsFontAwesome6.h"
#include "../TextureManager.h"

#include <fmt/format.h>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace adm::ui {

namespace {

constexpr int kRes = 64; // preview canvas resolution (matches the cheat sheet)

constexpr const char* kAnd = "\xC2\xB7"; // middle dot U+00B7

// The minterm bit for an A/B/C combination, mirroring the hardware: index = A*4 + B*2 + C.
inline int Index(bool a, bool b, bool c) { return (a ? 4 : 0) | (b ? 2 : 0) | (c ? 1 : 0); }

// Does the logic function actually depend on a given input? (i.e. is that DMA channel needed)
inline bool DependsOn(uint8_t m, int varBit /* 4=A 2=B 1=C */)
{
    for (int n = 0; n < 8; ++n)
        if (((m >> n) & 1) != ((m >> (n ^ varBit)) & 1))
            return true;
    return false;
}

// One literal of a product term: A/B/C, optionally complemented.
std::string TermString(uint8_t care, uint8_t val)
{
    static const char* names[3] = {"A", "B", "C"}; // bit2=A, bit1=B, bit0=C
    std::string out;
    for (int b = 2; b >= 0; --b) {
        int bit = 1 << b;
        if (!(care & bit))
            continue;
        if (!out.empty())
            out += kAnd;
        if (!(val & bit))
            out += "!";
        out += names[2 - b];
    }
    return out;
}

// Shape of each source channel, reproducing the cheat sheet's reference images.
// A = wedge, B = filled circle, C = horizontal stripes. true = pixel "lit".
bool ShapeA(int x, int y) { return std::abs(x - kRes / 2) < y / 2; }
bool ShapeB(int x, int y)
{
    int dx = x - kRes / 2, dy = y - kRes / 2;
    return dx * dx + dy * dy < 25 * 25;
}
bool ShapeC(int /*x*/, int y) { return (y & 7) == 0; }

// RGBA colours used in the preview.
inline void PutLit(uint8_t* p)
{
    p[0] = 0;
    p[1] = 180;
    p[2] = 0;
    p[3] = 255;
}
inline void PutOff(uint8_t* p)
{
    p[0] = 255;
    p[1] = 255;
    p[2] = 255;
    p[3] = 255;
}

// 1px black frame around a kRes x kRes RGBA buffer.
void DrawBorder(std::vector<uint8_t>& buf)
{
    auto black = [&](int x, int y) {
        uint8_t* p = &buf[(y * kRes + x) * 4];
        p[0] = p[1] = p[2] = 0;
        p[3] = 255;
    };
    for (int i = 0; i < kRes; ++i) {
        black(i, 0);
        black(i, kRes - 1);
        black(0, i);
        black(kRes - 1, i);
    }
}

// Build a source channel texture buffer. When the channel is off the hardware still feeds
// whatever happens to be in BLTxDAT, so we show noise to signal "value is unknown".
std::vector<uint8_t> BuildSource(bool (*shape)(int, int), bool on, std::mt19937& rng)
{
    std::vector<uint8_t> buf(kRes * kRes * 4);
    std::uniform_int_distribution<int> noise(200, 255);
    for (int y = 0; y < kRes; ++y) {
        for (int x = 0; x < kRes; ++x) {
            uint8_t* p = &buf[(y * kRes + x) * 4];
            if (on) {
                shape(x, y) ? PutLit(p) : PutOff(p);
            } else {
                p[0] = p[1] = p[2] = static_cast<uint8_t>(noise(rng));
                p[3] = 255;
            }
        }
    }
    DrawBorder(buf);
    return buf;
}

} // namespace

BlitterMintermView::~BlitterMintermView() { ReleaseTextures(); }

void BlitterMintermView::ReleaseTextures()
{
    if (!m_textures)
        return;
    for (unsigned int* t : {&m_texA, &m_texB, &m_texC, &m_texD}) {
        if (*t) {
            m_textures->ReleaseTexture(*t);
            *t = 0;
        }
    }
}

void BlitterMintermView::SetMinterm(uint8_t minterm)
{
    m_minterm = minterm;
    if (m_autoChannels)
        SyncAutoChannels();
    MarkDirty();
}

void BlitterMintermView::LoadFromBlit(uint16_t bltcon0, uint16_t bltcon1)
{
    ApplyBltcon(bltcon0, bltcon1);
    m_autoChannels = false; // honour the blit's exact USE flags
    Open();
}

void BlitterMintermView::SyncToBlit(uint16_t bltcon0, uint16_t bltcon1)
{
    if (!m_sync || !m_isOpen)
        return;

    // De-duplicate against what the window is currently showing, not against the last words
    // pushed at it. Selecting a blit has to undo any editing done since the previous sync, and
    // consecutive blits very often share BLTCON0/BLTCON1 -- a remembered pair suppressed that
    // reset outright. Both accessors reproduce every bit of the word they were given, so this
    // is an exact comparison; when it holds, ApplyBltcon would be a no-op anyway.
    if (!m_autoChannels && Bltcon0() == bltcon0 && Bltcon1() == bltcon1)
        return;

    ApplyBltcon(bltcon0, bltcon1);
    m_autoChannels = false; // mirror the blit's exact USE flags
}

void BlitterMintermView::SyncAutoChannels()
{
    m_useA = DependsOn(m_minterm, 4);
    m_useB = DependsOn(m_minterm, 2);
    m_useC = DependsOn(m_minterm, 1);
    m_useD = true; // you (almost) always need a destination
}

uint16_t BlitterMintermView::Bltcon0() const
{
    uint16_t v = static_cast<uint16_t>((m_ash & 0xF) << 12);
    if (m_useA)
        v |= 0x0800;
    if (m_useB)
        v |= 0x0400;
    if (m_useC)
        v |= 0x0200;
    if (m_useD)
        v |= 0x0100;
    v |= m_minterm;
    return v;
}

uint16_t BlitterMintermView::Bltcon1() const
{
    uint16_t v = static_cast<uint16_t>((m_bsh & 0xF) << 12);
    if (m_efe)
        v |= 0x0010;
    if (m_ife)
        v |= 0x0008;
    if (m_desc)
        v |= 0x0002;
    v |= m_con1Extra; // LINE, FCI/OVF and the line-mode octant bits the editor leaves alone
    return v;
}

void BlitterMintermView::ApplyBltcon(uint16_t bltcon0, uint16_t bltcon1)
{
    m_ash = (bltcon0 >> 12) & 0xF;
    m_useA = (bltcon0 & 0x0800) != 0;
    m_useB = (bltcon0 & 0x0400) != 0;
    m_useC = (bltcon0 & 0x0200) != 0;
    m_useD = (bltcon0 & 0x0100) != 0;
    m_minterm = static_cast<uint8_t>(bltcon0 & 0xFF);

    m_bsh = (bltcon1 >> 12) & 0xF;
    m_line = (bltcon1 & 0x0001) != 0;
    if (m_line) {
        // Line mode redefines bits 4-1 as SUD/SUL/AUL/SING. Decoding them as EFE/IFE/DESC would
        // put three wrong ticks on screen, so the fill flags read false and the bits ride along
        // untouched in m_con1Extra.
        m_efe = m_ife = m_desc = false;
        m_con1Extra = static_cast<uint16_t>(bltcon1 & 0x0FFF);
    } else {
        m_efe = (bltcon1 & 0x0010) != 0;
        m_ife = (bltcon1 & 0x0008) != 0;
        m_desc = (bltcon1 & 0x0002) != 0;
        m_con1Extra = static_cast<uint16_t>(bltcon1 & 0x0FE5); // low bits minus EFE/IFE/DESC
    }
    MarkDirty();
}

std::string BlitterMintermView::Expression(uint8_t m)
{
    if (m == 0x00)
        return "0";
    if (m == 0xFF)
        return "1";

    // Collect every prime-ish implicant: a product term (care/value over A,B,C) all of whose
    // covered rows are 1 in the function. Then greedily cover the ON-set, preferring terms with
    // fewer literals. With only 3 variables this is tiny and exact enough for a readable result.
    struct Imp
    {
        uint8_t cover;
        uint8_t care;
        uint8_t val;
        int literals;
    };
    std::vector<Imp> imps;
    for (uint8_t care = 0; care < 8; ++care) {
        for (uint8_t val = 0; val < 8; ++val) {
            if (val & ~care)
                continue;
            uint8_t cover = 0;
            bool ok = true;
            for (int n = 0; n < 8; ++n) {
                if ((n & care) == val) {
                    if (!((m >> n) & 1)) {
                        ok = false;
                        break;
                    }
                    cover |= static_cast<uint8_t>(1 << n);
                }
            }
            if (ok)
                imps.push_back({cover, care, val, __builtin_popcount(care)});
        }
    }

    uint8_t need = m;
    std::string out;
    while (need) {
        int best = -1, bestScore = -1, bestLit = 99;
        for (size_t i = 0; i < imps.size(); ++i) {
            int score = __builtin_popcount(imps[i].cover & need);
            if (score == 0)
                continue;
            if (score > bestScore || (score == bestScore && imps[i].literals < bestLit)) {
                bestScore = score;
                bestLit = imps[i].literals;
                best = static_cast<int>(i);
            }
        }
        if (best < 0)
            break;
        if (!out.empty())
            out += " + ";
        out += TermString(imps[best].care, imps[best].val);
        need &= ~imps[best].cover;
    }
    return out;
}

void BlitterMintermView::RebuildTextures()
{
    if (!m_textures)
        return;
    ReleaseTextures();

    std::mt19937 rng(0xB117); // fixed seed → stable noise (no per-frame flicker)

    auto srcA = BuildSource(ShapeA, m_useA, rng);
    auto srcB = BuildSource(ShapeB, m_useB, rng);
    auto srcC = BuildSource(ShapeC, m_useC, rng);

    // Destination = apply the minterm to the per-pixel A/B/C inputs (with A/B shifted). For an
    // off channel the input is unknown, so we feed random bits — the noise shows through in D.
    std::vector<uint8_t> dst(kRes * kRes * 4);
    std::bernoulli_distribution coin(0.5);
    if (m_useD) {
        for (int y = 0; y < kRes; ++y) {
            for (int x = 0; x < kRes; ++x) {
                int ax = x - m_ash;
                int bx = x - m_bsh;
                bool A = m_useA ? (ax >= 0 && ShapeA(ax, y)) : coin(rng);
                bool B = m_useB ? (bx >= 0 && ShapeB(bx, y)) : coin(rng);
                bool C = m_useC ? ShapeC(x, y) : coin(rng);
                bool lit = (m_minterm >> Index(A, B, C)) & 1;
                uint8_t* p = &dst[(y * kRes + x) * 4];
                lit ? PutLit(p) : PutOff(p);
            }
        }
    } else {
        std::uniform_int_distribution<int> noise(200, 255);
        for (auto& v : dst)
            v = static_cast<uint8_t>(noise(rng));
        for (int i = 0; i < kRes * kRes; ++i)
            dst[i * 4 + 3] = 255;
    }
    DrawBorder(dst);

    m_texA = m_textures->CreateTexture(srcA.data(), kRes, kRes);
    m_texB = m_textures->CreateTexture(srcB.data(), kRes, kRes);
    m_texC = m_textures->CreateTexture(srcC.data(), kRes, kRes);
    m_texD = m_textures->CreateTexture(dst.data(), kRes, kRes);
    m_visDirty = false;
}

void BlitterMintermView::Draw()
{
    if (!m_isOpen)
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocusOnAppearing = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 780), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_CALCULATOR " Blitter Minterm###Blitter Minterm", &m_isOpen, flags)) {
        ImGui::End();
        return;
    }

    // Top bar: Sync toggle (left) and an (i) info tooltip (right).
    ImGui::Checkbox("Sync", &m_sync);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "When on, the window follows the selected blit as you scrub the frame, click a "
            "Blitter-window row, or click a blit in the frame scrubber.\nWhen off, it only "
            "changes when you click a minterm hyperlink.");

    const char* kInfo = "(i)";
    float infoW = ImGui::CalcTextSize(kInfo).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - infoW);
    ImGui::TextDisabled("%s", kInfo);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(
            "The blitter combines sources A, B and C into destination D with an 8-bit logic "
            "function (the minterm). Each truth-table row picks whether D is set for that "
            "combination of inputs.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    DrawChannels();
    ImGui::Spacing();
    DrawTruthTable();
    ImGui::Spacing();
    DrawCanvases();
    ImGui::Spacing();
    DrawSummary();
    ImGui::Separator();
    DrawPresets();
    ImGui::Spacing();
    DrawBltcon();

    if (m_visDirty)
        RebuildTextures();

    ImGui::End();
}

void BlitterMintermView::DrawChannels()
{
    ImGui::SeparatorText("DMA channels");

    if (ImGui::Checkbox("Auto", &m_autoChannels)) {
        if (m_autoChannels)
            SyncAutoChannels();
        MarkDirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Derive USEA/USEB/USEC from the inputs the minterm actually uses.");

    ImGui::SameLine(0.0f, 20.0f);
    ImGui::BeginDisabled(m_autoChannels);
    if (ImGui::Checkbox("A", &m_useA))
        MarkDirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("B", &m_useB))
        MarkDirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("C", &m_useC))
        MarkDirty();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("D (dest)", &m_useD))
        MarkDirty();

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("A shift", &m_ash, 0, 15))
        MarkDirty();
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("B shift", &m_bsh, 0, 15))
        MarkDirty();
}

void BlitterMintermView::DrawTruthTable()
{
    ImGui::SeparatorText("Minterm truth table");

    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##minterm", 6, flags))
        return;

    ImGui::TableSetupColumn("Bit", ImGuiTableColumnFlags_WidthFixed, 36.0f);
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("C", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Term", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableHeadersRow();

    static const ImVec4 kGreen = {0.30f, 0.80f, 0.30f, 1.0f};
    static const ImVec4 kGrey = {0.55f, 0.55f, 0.55f, 1.0f};

    // n = 0 (000) at top down to 7 (111), matching the cheat sheet.
    for (int n = 0; n < 8; ++n) {
        bool a = (n >> 2) & 1, b = (n >> 1) & 1, c = n & 1;
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Text("LF%d", n);

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", a);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", b);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d", c);

        // Product-term label: lit input = green capital, clear input = grey lowercase.
        ImGui::TableSetColumnIndex(4);
        ImGui::TextColored(a ? kGreen : kGrey, "%s", a ? "A" : "a");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(b ? kGreen : kGrey, "%s", b ? "B" : "b");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(c ? kGreen : kGrey, "%s", c ? "C" : "c");

        ImGui::TableSetColumnIndex(5);
        bool d = (m_minterm >> n) & 1;
        ImGui::PushID(n);
        if (ImGui::Checkbox("##d", &d)) {
            if (d)
                m_minterm |= static_cast<uint8_t>(1 << n);
            else
                m_minterm &= static_cast<uint8_t>(~(1 << n));
            if (m_autoChannels)
                SyncAutoChannels();
            MarkDirty();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void BlitterMintermView::DrawCanvases()
{
    ImGui::SeparatorText("Preview");

    const float sz = 96.0f;
    struct Canvas
    {
        const char* label;
        unsigned int tex;
        bool on;
    };
    const Canvas canvases[4] = {
        {"A", m_texA, m_useA},
        {"B", m_texB, m_useB},
        {"C", m_texC, m_useC},
        {"D", m_texD, m_useD},
    };

    if (ImGui::BeginTable("##preview", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        for (int i = 0; i < 4; ++i) {
            ImGui::TableSetColumnIndex(i);
            const Canvas& cv = canvases[i];
            if (cv.tex)
                ImGui::Image(TextureManager::ToImTextureID(cv.tex), ImVec2(sz, sz));
            else
                ImGui::Dummy(ImVec2(sz, sz));
        }
        ImGui::TableNextRow();
        for (int i = 0; i < 4; ++i) {
            ImGui::TableSetColumnIndex(i);
            const Canvas& cv = canvases[i];
            if (i == 3)
                ImGui::TextColored({0.4f, 0.8f, 1.0f, 1.0f}, "%s = result", cv.label);
            else if (cv.on)
                ImGui::Text("%s", cv.label);
            else
                ImGui::TextDisabled("%s (off)", cv.label);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("A wedge, B circle, C stripes \xE2\x80\x94 lit pixels in green.");
}

void BlitterMintermView::DrawSummary()
{
    ImGui::SeparatorText("Logic function");
    ImGui::Text("Minterm: ");
    ImGui::SameLine();
    ImGui::TextColored({0.4f, 0.8f, 1.0f, 1.0f}, "$%02X", m_minterm);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Text("%c%c%c%c%c%c%c%c", (m_minterm & 0x80) ? '1' : '0', (m_minterm & 0x40) ? '1' : '0',
                (m_minterm & 0x20) ? '1' : '0', (m_minterm & 0x10) ? '1' : '0',
                (m_minterm & 0x08) ? '1' : '0', (m_minterm & 0x04) ? '1' : '0',
                (m_minterm & 0x02) ? '1' : '0', (m_minterm & 0x01) ? '1' : '0');

    ImGui::Text("D = ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored({0.30f, 0.80f, 0.30f, 1.0f}, "%s", Expression(m_minterm).c_str());
}

void BlitterMintermView::DrawPresets()
{
    ImGui::SeparatorText("Common operations");

    // name, BLTCON0, BLTCON1 — same canonical pairs the Blitter view recognises.
    struct Preset
    {
        const char* name;
        uint16_t bltcon0;
        uint16_t bltcon1;
        const char* tip;
    };
    static const Preset presets[] = {
        {"clear", 0x0100, 0x0000, "D = 0 (zero-fill the destination)"},
        {"copy", 0x09F0, 0x0000, "D = A (straight copy of source A)"},
        {"fill", 0x09F0, 0x0012, "Area fill (inclusive) of A"},
        {"xfill", 0x09F0, 0x0008, "Area fill (exclusive) of A"},
        {"mask", 0x0DC0, 0x0000, "D = A AND B (mask A through B)"},
        {"cookie", 0x0FCA, 0x0000,
         "D = A"
         "\xC2\xB7"
         "B + !A"
         "\xC2\xB7"
         "C (cookie-cut bob)"},
        {"bob", 0x0FEA, 0x0000, "Blit a masked bob over a background"},
    };

    int perRow = 4;
    for (int i = 0; i < static_cast<int>(std::size(presets)); ++i) {
        const Preset& p = presets[i];
        if (ImGui::Button(p.name, ImVec2(76.0f, 0.0f))) {
            ApplyBltcon(p.bltcon0, p.bltcon1);
            m_autoChannels = false; // preset defines the exact channels
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nBLTCON0 $%04X  BLTCON1 $%04X", p.tip, p.bltcon0, p.bltcon1);
        if ((i % perRow) != perRow - 1 && i != static_cast<int>(std::size(presets)) - 1)
            ImGui::SameLine();
    }
}

void BlitterMintermView::DrawBltcon()
{
    ImGui::SeparatorText("Control words");

    uint16_t con0 = Bltcon0();
    uint16_t con1 = Bltcon1();

    // BLTCON0 hex field, round-tripped through the editable state.
    char buf0[16];
    std::snprintf(buf0, sizeof(buf0), "%04X", con0);
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputText("BLTCON0", buf0, sizeof(buf0),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
        ApplyBltcon(static_cast<uint16_t>(std::strtoul(buf0, nullptr, 16)), con1);
    }
    ImGui::SameLine(0.0f, 16.0f);
    char buf1[16];
    std::snprintf(buf1, sizeof(buf1), "%04X", con1);
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputText("BLTCON1", buf1, sizeof(buf1),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
        ApplyBltcon(con0, static_cast<uint16_t>(std::strtoul(buf1, nullptr, 16)));
    }

    // BLTCON0 field breakdown.
    ImGui::TextDisabled("BLTCON0: ASH=%d  USE %s%s%s%s  LF=$%02X", m_ash, m_useA ? "A" : "-",
                        m_useB ? "B" : "-", m_useC ? "C" : "-", m_useD ? "D" : "-", m_minterm);

    // BLTCON1 fill / direction options. A line-draw blit spends the same bits on the octant
    // fields, so they are shown disabled rather than decoded into fill flags that would be wrong.
    if (m_line) {
        ImGui::TextColored({1.0f, 0.6f, 0.1f, 1.0f}, "LINE");
        ImGui::SameLine();
        ImGui::TextDisabled("bits 4-1 are SUD/SUL/AUL/SING here, not fill flags");
    }
    ImGui::BeginDisabled(m_line);
    if (ImGui::Checkbox("EFE (exclusive fill)", &m_efe))
        MarkDirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("IFE (inclusive fill)", &m_ife))
        MarkDirty();
    if (ImGui::Checkbox("DESC (descending)", &m_desc))
        MarkDirty();
    ImGui::EndDisabled();
}

} // namespace adm::ui
