#include "ExocetVideoComposer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <fmt/format.h>
#include <glad/glad.h>
#include <imgui_impl_opengl3.h>

#include "../core/Constants.h"
#include "../core/CopperRaster.h"
#include "../core/ExocetSession.h"
#include "../core/PaulaStateSnapshot.h"
#include "DmaColors.h"
#include "DriveChips.h"
#include "views/EmulatorView.h"
#include "views/ExocetSong.h"
#include "views/ExocetTimeline.h"
#include "views/ExocetUi.h"

namespace adm::ui {

namespace {

// The interface text's height in the picture: the app's look, sized for a 4K screen
constexpr float kFontPx = 26.0f;
// The smallest it goes where the room is short
constexpr float kMinFontPx = 14.0f;
// Between the panels and round the edge
constexpr float kMargin = 24.0f;
// The room for the Amiga's picture: a PAL frame (720x574 at its shape) shown twice over
constexpr float kPictureW = 1440.0f;
constexpr float kPictureH = 1148.0f;

constexpr ImVec4 kPlayTint = {0.20f, 0.75f, 0.35f, 0.38f};
constexpr ImVec4 kPatternTint = {1.00f, 1.00f, 1.00f, 0.05f};
constexpr ImVec4 kKeyColour = {1.00f, 0.92f, 0.55f, 1.00f};

// The drive lamp, as the Amiga Emulator window's status bar has it: dim idle, green reading,
// red writing
constexpr ImVec4 kLampIdle = {0.28f, 0.50f, 0.28f, 1.0f};
constexpr ImVec4 kLampRead = {0.45f, 0.95f, 0.45f, 1.0f};
constexpr ImVec4 kLampWrite = {1.00f, 0.35f, 0.35f, 1.0f};
// How much of a track's glow is left after a frame: about two seconds to fade
constexpr float kHeatFade = 0.955f;
// The transfers listed: the newest, as many as the panel has lines for
constexpr size_t kTransfersKept = 64;

// Paula's voices, the colours the Audio window gives them
constexpr ImU32 kVoiceColour[4] = {
    IM_COL32(220, 200, 60, 255),
    IM_COL32(80, 200, 200, 255),
    IM_COL32(200, 120, 200, 255),
    IM_COL32(120, 200, 120, 255),
};
// The samples pushed for a frame a voice played nothing in, so its trace runs flat rather than
// holding the last thing it played
constexpr int kSilencePush = 128;

// The DMA slots drawn, in the legend's order, and what they are called there
constexpr int kDmaKinds[] = {2, 3, 5, 6, 7, 8, 4, 1, 9};

// The GL state the composer touches outside what the ImGui backend saves and restores itself
struct GlStateGuard
{
    GLint drawFb = 0, readFb = 0, texture = 0, packBuffer = 0, unpackBuffer = 0;
    GLint packAlign = 4, packRow = 0, unpackAlign = 4, unpackRow = 0;
    GLfloat clear[4] = {};
    GLboolean scissor = GL_FALSE;

    GlStateGuard()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &packRow);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlign);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRow);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    ~GlStateGuard()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFb));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFb));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer));
        glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
        glPixelStorei(GL_PACK_ROW_LENGTH, packRow);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRow);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
    }
};

// Makes `context` current for the scope, and the one before it again after
struct ContextScope
{
    ImGuiContext* previous;
    explicit ContextScope(ImGuiContext* context) : previous(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

// 12,345
std::string Thousands(uint32_t n)
{
    std::string d = std::to_string(n);
    for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3)
        d.insert(static_cast<size_t>(i), ",");
    return d;
}

// A track's name without its group's, which the bar above it says: "tunnel.speed" -> "speed"
std::string ShortName(const std::string& name)
{
    const auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

} // namespace

ExocetVideoComposer::ExocetVideoComposer(core::ExocetSession* session, EmulatorView* emulatorView)
    : m_session(session), m_emulatorView(emulatorView), m_memory(session)
{
}

ExocetVideoComposer::~ExocetVideoComposer()
{
    if (m_context) {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_context);
        // The shared font atlas's textures stay: the backend frees only those no other context
        // uses (their RefCount), and the application's context does.
        if (m_backend)
            ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(previous != m_context ? previous : nullptr);
    }
    if (m_framebuffer)
        glDeleteFramebuffers(1, &m_framebuffer);
    if (m_target)
        glDeleteTextures(1, &m_target);
    if (m_frame)
        glDeleteTextures(1, &m_frame);
    if (m_dmaTexture)
        glDeleteTextures(1, &m_dmaTexture);
}

bool ExocetVideoComposer::EnsureResources()
{
    if (!m_context) {
        // A context of its own, so nothing it draws touches the application's windows, with the
        // application's fonts and look, enlarged for the picture
        ImGuiContext* main = ImGui::GetCurrentContext();
        if (!main) {
            m_error = "there is no ImGui context to take the fonts from";
            return false;
        }
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        ImFont* font = ImGui::GetIO().FontDefault;
        ImGuiStyle style = ImGui::GetStyle();
        m_context = ImGui::CreateContext(atlas);
        ContextScope scope(m_context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.FontDefault = font;
        io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
        const float shown = style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi;
        style.ScaleAllSizes(kFontPx / (shown > 0.0f ? shown : 13.0f));
        style.FontSizeBase = kFontPx;
        style.FontScaleMain = 1.0f;
        style.FontScaleDpi = 1.0f;
        ImGui::GetStyle() = style;
        m_backend = ImGui_ImplOpenGL3_Init("#version 330");
        if (!m_backend) {
            m_error = "the OpenGL renderer would not start";
            return false;
        }
    }
    if (!m_framebuffer) {
        glGenTextures(1, &m_target);
        glBindTexture(GL_TEXTURE_2D, m_target);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &m_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_target, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            m_error = fmt::format("the {}x{} framebuffer is not complete", kWidth, kHeight);
            return false;
        }
    }
    if (!m_frame) {
        glGenTextures(1, &m_frame);
        glBindTexture(GL_TEXTURE_2D, m_frame);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return true;
}

void ExocetVideoComposer::UploadFrame(const uint8_t* rgba, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, m_frame);
    if (width != m_frameWidth || height != m_frameHeight) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        m_frameWidth = width;
        m_frameHeight = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

bool ExocetVideoComposer::Compose(const uint8_t* rgba, int width, int height, int scaleX,
                                  int scaleY, std::vector<uint8_t>& out)
{
    m_error.clear();
    if (!rgba || width <= 0 || height <= 0) {
        m_error = "no frame";
        return false;
    }
    GlStateGuard guard;
    if (!EnsureResources())
        return false;
    UploadFrame(rgba, width, height);
    TakeFrame();

    ContextScope scope(m_context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 50.0f;
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    // The first frame of a new context lays its windows and tables out; draw it twice so the
    // film's first frame is already the settled one
    for (int pass = m_warm ? 1 : 0; pass < 2; ++pass) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        DrawPanels(width, height, std::max(1, scaleX), std::max(1, scaleY));
        ImGui::Render();
    }
    m_warm = true;

    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glDisable(GL_SCISSOR_TEST);
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_DockingEmptyBg);
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // GL's rows run bottom up; the film's top down
    const size_t rowBytes = static_cast<size_t>(kWidth) * 4;
    m_readback.resize(rowBytes * kHeight);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, m_readback.data());
    out.resize(m_readback.size());
    for (int y = 0; y < kHeight; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * rowBytes,
                    m_readback.data() + static_cast<size_t>(kHeight - 1 - y) * rowBytes, rowBytes);
    return true;
}

void ExocetVideoComposer::DrawPanels(int width, int height, int scaleX, int scaleY)
{
    static const core::ExocetBlockState kNoBlock;
    const core::ExocetBlockState* live = m_session->LiveBlock();
    const core::ExocetBlockState& block = live ? *live : kNoBlock;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float titleH = ImGui::GetFrameHeight();
    const ImGuiWindowFlags panel =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    const float W = static_cast<float>(kWidth), H = static_cast<float>(kHeight);

    // The Amiga's picture, top left, in a panel whose size never changes - a film from a cold
    // boot goes from the Kickstart's screen to the demo's mode, and the panels around it stay
    // put. Room for a PAL frame shown twice over; the picture enlarged by whole numbers as far
    // as it fits, so every pixel stays a sharp block, at the shape the emulator window shows it
    const float baseW = static_cast<float>(width * scaleX);
    const float baseH = static_cast<float>(height * scaleY);
    float k = std::floor(std::min(kPictureW / baseW, kPictureH / baseH));
    if (k < 1.0f)
        k = std::min(kPictureW / baseW, kPictureH / baseH);
    const ImVec2 image(baseW * k, baseH * k);
    const ImVec2 pad = style.WindowPadding;
    const float topH = std::max(kPictureH + pad.y * 2.0f + titleH, H * 0.5f);
    const float amigaW = kPictureW + pad.x * 2.0f;

    ImGui::SetNextWindowPos(ImVec2(kMargin, kMargin));
    ImGui::SetNextWindowSize(ImVec2(amigaW, topH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("Amiga", nullptr, panel)) {
        ImGui::SetCursorPos(ImVec2(pad.x + (kPictureW - image.x) * 0.5f,
                                   titleH + (topH - titleH - image.y) * 0.5f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
        ImGui::Image(static_cast<ImTextureID>(m_frame), image);
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
    }
    ImGui::End();

    // The grid, top right, with where the demo is in its title
    const float gridX = kMargin * 2.0f + amigaW;
    std::string title = "Exocet Grid";
    if (block.present)
        title += fmt::format("   row {}   frame {}   {}", block.row, block.frame,
                             exo::SongTime(block.frame, m_session->Fps()));
    ImGui::SetNextWindowPos(ImVec2(gridX, kMargin));
    ImGui::SetNextWindowSize(ImVec2(W - gridX - kMargin, topH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin((title + "###grid").c_str(), nullptr, panel))
        DrawGrid(block);
    ImGui::End();

    // Along the bottom: the timeline; the memory's bars over the drives; Paula over the DMA
    const float bottomY = kMargin * 2.0f + topH;
    const float bottomH = H - bottomY - kMargin;
    const float timelineW = std::floor((W - kMargin * 4.0f) * 0.4f);
    const float columnW = std::floor((W - kMargin * 4.0f - timelineW) * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(kMargin, bottomY));
    ImGui::SetNextWindowSize(ImVec2(timelineW, bottomH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("Timeline", nullptr, panel)) {
        // Smaller text when the lanes - a part each and a group each - would not fit at the
        // panel's size: the legend and the time axis take about three lines more
        const auto* pack = m_session->Pack();
        const int lanes = static_cast<int>(m_session->PartNames().size()) +
                          (pack ? static_cast<int>(pack->Groups().size()) : 0);
        const float room = ImGui::GetContentRegionAvail().y;
        const float fits = room / (static_cast<float>(lanes) + 4.0f) - 6.0f;
        ImGui::PushFont(nullptr, std::clamp(fits, kMinFontPx, kFontPx));
        exo::DrawTimeline(*m_session, block, true);
        ImGui::PopFont();
    }
    ImGui::End();

    // The memory as tall as its bars: the counts line, a line and a bar a region, the legend
    const float memoryX = kMargin * 2.0f + timelineW;
    const core::ExocetMemoryState* mem = m_session->Memory();
    const int regions = mem && mem->present ? static_cast<int>(mem->regions.size()) : 2;
    const float line = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    const float bar = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const float memoryH =
        std::min(bottomH * 0.5f, titleH + pad.y * 2.0f + line * static_cast<float>(2 + regions) +
                                     bar * static_cast<float>(regions));
    ImGui::SetNextWindowPos(ImVec2(memoryX, bottomY));
    ImGui::SetNextWindowSize(ImVec2(columnW, memoryH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("Exocet Memory", nullptr, panel))
        m_memory.DrawContents(false);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(memoryX, bottomY + memoryH + kMargin));
    ImGui::SetNextWindowSize(ImVec2(columnW, bottomH - memoryH - kMargin));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("Drives", nullptr, panel))
        DrawDrives();
    ImGui::End();

    const float chipsX = memoryX + columnW + kMargin;
    const float chipsW = W - chipsX - kMargin;
    const float paulaH = std::floor((bottomH - kMargin) * 0.42f);
    ImGui::SetNextWindowPos(ImVec2(chipsX, bottomY));
    ImGui::SetNextWindowSize(ImVec2(chipsW, paulaH));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("Paula", nullptr, panel))
        DrawPaula();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(chipsX, bottomY + paulaH + kMargin));
    ImGui::SetNextWindowSize(ImVec2(chipsW, bottomH - paulaH - kMargin));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("DMA", nullptr, panel))
        DrawDma();
    ImGui::End();
}

// The grid as the Exocet Grid draws it, read-only and following the playhead: the song's order
// row over the row, its frame and time, the song's channels, and the tracks of the groups the
// demo has enabled, by group. Drawn by hand rather than as a table, so the playhead's row sits at
// the same height every frame.
void ExocetVideoComposer::DrawGrid(const core::ExocetBlockState& b)
{
    auto* pack = m_session->Pack();
    const auto& check = m_session->Check();
    if (!pack || !check.valid || check.rows.empty()) {
        ImGui::TextDisabled("The song's rows are not known yet: check the song");
        return;
    }
    const auto& rows = check.rows;
    const auto& groups = pack->Groups();
    const auto& tracks = pack->Tracks();
    const auto& activity = m_session->Activity();
    const int nrows = static_cast<int>(rows.size());
    const bool live = b.present && b.manifest == pack->ManifestHash();
    const int playRow = live && b.row < nrows ? static_cast<int>(b.row) : -1;
    const int channels = std::clamp(check.channels, 0, 4);
    const bool songs = check.digests.size() > 1;
    const int fps = m_session->Fps();

    // The order row above it all, as a tracker has it over the pattern
    exo::DrawOrderList(check, playRow, fps);

    // The tracks of the groups the demo has enabled at this frame - the running part's, every one,
    // keyed or not, so its values show - a group at a time in the manifest's order, the
    // director's first. None before the block appears.
    std::vector<int> groupOrder;
    for (size_t g = 0; g < groups.size() && g < 32; ++g) {
        if (!live || !(b.enabled & (1u << g)))
            continue;
        if (groups[g].name == "director")
            groupOrder.insert(groupOrder.begin(), static_cast<int>(g));
        else
            groupOrder.push_back(static_cast<int>(g));
    }
    struct Column
    {
        int track, group;
    };
    std::vector<Column> columns;
    for (int g : groupOrder)
        for (size_t t = 0; t < tracks.size(); ++t)
            if (tracks[t].group == groups[g].name)
                columns.push_back({static_cast<int>(t), g});

    // The leading columns, then as many tracks as fit, the rest counted at the end. A track's
    // lane is half the Exocet Grid's - half the widest value it could hold - so twice as many
    // show; what is too wide for one is drawn smaller
    const ImGuiStyle& st = ImGui::GetStyle();
    const float padX = st.CellPadding.x * 2.0f + 4.0f;
    const float laneW = std::floor((ImGui::CalcTextSize("-0000.000 ~").x + padX) * 0.5f);
    const float rowW = ImGui::CalcTextSize(songs ? "0 00:00.0" : "00:00.0").x + padX;
    const float frameW = ImGui::CalcTextSize("00000").x + padX;
    const float timeW = ImGui::CalcTextSize("00:00.00").x + padX;
    const float chW = ImGui::CalcTextSize("C-3 05 A0F").x + padX;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float right = origin.x + avail.x;
    const float leadW = rowW + frameW + timeW + chW * static_cast<float>(channels);
    const float moreW = ImGui::CalcTextSize("+00 more").x + padX;
    size_t shown = columns.size();
    const float room = right - origin.x - leadW;
    if (laneW * static_cast<float>(shown) > room)
        shown = static_cast<size_t>(std::max(0.0f, (room - moreW) / laneW));
    const int hidden = static_cast<int>(columns.size() - shown);

    // Each lane's name, on two lines where one is too narrow for it and it has an underscore to
    // break at - the last that leaves the first line fitting, else the first - the underscore
    // dropped. A word is never split: one without an underscore is drawn smaller instead
    const float nameRoom = laneW - st.CellPadding.x * 2.0f;
    std::vector<std::array<std::string, 2>> names(shown);
    bool twoLines = false;
    for (size_t i = 0; i < shown; ++i) {
        const std::string s = ShortName(tracks[columns[i].track].name);
        names[i][1] = s;
        if (ImGui::CalcTextSize(s.c_str()).x <= nameRoom)
            continue;
        size_t fit = 1;
        while (fit < s.size() && ImGui::CalcTextSize(s.substr(0, fit + 1).c_str()).x <= nameRoom)
            ++fit;
        size_t under = s.rfind('_', fit);
        if (under == std::string::npos || under == 0)
            under = s.find('_', 1);
        if (under == std::string::npos)
            continue;
        names[i] = {s.substr(0, under), s.substr(under + 1)};
        twoLines = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = lineH + st.CellPadding.y * 2.0f;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 key = ImGui::GetColorU32(kKeyColour);
    const ImU32 headerBg = ImGui::GetColorU32(ImGuiCol_TableHeaderBg);
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_TableBorderLight);
    const ImU32 dark = IM_COL32(20, 20, 24, 255);
    const float bottom = origin.y + avail.y;
    // Text too wide for its cell drawn smaller, down to kMinFontPx, and cut at the edge past that.
    // Measured again at the smaller size, which does not scale quite in proportion
    auto cellText = [&](float x0, float x1, float y, ImU32 col, const std::string& s) {
        const float fits = x1 - x0 - st.CellPadding.x * 2.0f;
        float size = fontSize;
        for (int pass = 0; pass < 3 && size > kMinFontPx; ++pass) {
            const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x;
            if (w <= fits)
                break;
            size = std::max(kMinFontPx, std::floor(size * fits / w));
        }
        const ImVec4 clip(x0, y, x1 - st.CellPadding.x, y + rowH);
        dl->AddText(font, size, ImVec2(x0 + st.CellPadding.x, y + (rowH - size) * 0.5f), col,
                    s.c_str(), nullptr, 0.0f, &clip);
    };

    // The x of every column's left edge, and the end
    std::vector<float> xs;
    float x = origin.x;
    const float lead[3] = {rowW, frameW, timeW};
    for (float w : lead) {
        xs.push_back(x);
        x += w;
    }
    for (int c = 0; c < channels; ++c) {
        xs.push_back(x);
        x += chW;
    }
    const size_t firstTrack = xs.size();
    for (size_t i = 0; i < shown; ++i) {
        xs.push_back(x);
        x += laneW;
    }
    const float tableRight = hidden ? x + moreW : x;
    xs.push_back(x);

    // The group row: a bar in each group's colour over its columns, its name on it; under it the
    // names, a second line where one breaks, and the first row of the grid at y2
    const float y0 = origin.y, y1 = y0 + rowH, yName = y1 + (twoLines ? rowH : 0.0f);
    const float y2 = yName + rowH;
    dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(tableRight, y2), headerBg);
    for (size_t i = 0; i < shown;) {
        size_t j = i;
        while (j < shown && columns[j].group == columns[i].group)
            ++j;
        const float a = xs[firstTrack + i], z = xs[firstTrack + j];
        dl->AddRectFilled(ImVec2(a, y0), ImVec2(z - 1, y1), exo::GroupColour(columns[i].group));
        dl->AddRectFilled(ImVec2(a, y1), ImVec2(z - 1, y2),
                          exo::GroupColour(columns[i].group, 150));
        cellText(a, z, y0, dark, groups[columns[i].group].name);
        i = j;
    }
    // The names
    cellText(xs[0], xs[1], yName, text, "Row");
    cellText(xs[1], xs[2], yName, text, "Frame");
    cellText(xs[2], xs[3], yName, text, "Time");
    const auto& lights = m_session->Lights();
    for (int c = 0; c < channels; ++c) {
        const float a = xs[3 + c], z = xs[4 + c];
        cellText(a, z, yName, text, fmt::format("Ch {}", c + 1));
        // the channel's light: a trigger flashes it, the level holds a glow under it
        const ImVec2 at(z - rowH * 0.5f - 2.0f, yName + rowH * 0.5f);
        const float rad = rowH * 0.3f;
        dl->AddCircleFilled(at, rad, IM_COL32(40, 50, 40, 255));
        const float lit = std::max(lights[c].trigger, lights[c].level * 0.6f);
        if (lit > 0.02f)
            dl->AddCircleFilled(at, rad, IM_COL32(90, 255, 120, static_cast<int>(255 * lit)));
    }
    for (size_t i = 0; i < shown; ++i) {
        const float a = xs[firstTrack + i], z = xs[firstTrack + i + 1];
        if (!names[i][0].empty())
            cellText(a, z, y1, text, names[i][0]);
        cellText(a, z, yName, text, names[i][1]);
    }
    if (hidden)
        cellText(xs.back(), tableRight, yName, dim, fmt::format("+{} more", hidden));

    // The rows, the playhead's two fifths of the way down
    const int visible = std::max(1, static_cast<int>((bottom - y2) / rowH));
    int first = playRow >= 0 ? playRow - visible * 2 / 5 : 0;
    first = std::clamp(first, 0, std::max(0, nrows - visible));
    for (int line = 0; line < visible && first + line < nrows; ++line) {
        const int i = first + line;
        const auto& r = rows[i];
        const float y = y2 + rowH * static_cast<float>(line);
        if (i == playRow)
            dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(tableRight, y + rowH),
                              ImGui::GetColorU32(kPlayTint));
        else if (r.row == 0)
            dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(tableRight, y + rowH),
                              ImGui::GetColorU32(kPatternTint));
        cellText(xs[0], xs[1], y, r.row % 4 == 0 ? text : dim, core::ExocetPlaceLabel(r, songs));
        cellText(xs[1], xs[2], y, dim, std::to_string(r.frame));
        cellText(xs[2], xs[3], y, dim, exo::SongTime(r.frame, fps));
        for (int c = 0; c < channels; ++c) {
            const bool any = c < static_cast<int>(r.cells.size()) &&
                             (r.cells[c][0] || r.cells[c][1] || r.cells[c][2] || r.cells[c][3]);
            cellText(xs[3 + c], xs[4 + c], y, any ? text : dim,
                     any ? exo::Cell(r.cells[c]) : std::string("--- .. ..."));
        }
        for (size_t col = 0; col < shown; ++col) {
            const int t = columns[col].track;
            const auto& info = tracks[t];
            const float a = xs[firstTrack + col], z = xs[firstTrack + col + 1];
            if (activity.At(info.group, i) == core::ExocetActivity::State::Inactive)
                dl->AddRectFilled(ImVec2(a, y), ImVec2(z, y + rowH), IM_COL32(0, 0, 0, 70));
            else
                dl->AddRectFilled(ImVec2(a, y), ImVec2(z, y + rowH),
                                  exo::GroupColour(columns[col].group, 22));
            const auto& keys = m_session->HostKeys(t);
            const auto k = std::find_if(keys.begin(), keys.end(), [&](const core::ExocetKey& e) {
                return e.row == i && e.sub == 0;
            });
            const bool isColour = exo::TypeOf(info) == core::ExocetType::Color;
            int32_t value = 0;
            std::string s;
            ImU32 col32 = dim;
            if (k != keys.end()) {
                value = k->value;
                s = exo::FormatValue(info, value) + " " + exo::Glyph(k->interp);
                col32 = key;
            } else if (exo::TypeOf(info) == core::ExocetType::Event) {
                s = ".";
            } else {
                value = m_session->HostValue(t, static_cast<uint32_t>(i), 0);
                s = exo::FormatValue(info, value);
            }
            // A colour's lane too narrow for a swatch beside its number: the swatch fills the
            // cell, outlined in the key's colour on a key, the number written on it dark on a
            // light colour and light on a dark one
            float inset = 0.0f;
            if (isColour) {
                const auto c = static_cast<uint32_t>(value);
                const int red = c >> 24, green = (c >> 16) & 255, blue = (c >> 8) & 255,
                          alpha = c & 255;
                const ImVec2 lo(a + st.CellPadding.x, y + st.CellPadding.y);
                const ImVec2 hi(z - st.CellPadding.x, y + rowH - st.CellPadding.y);
                dl->AddRectFilled(lo, hi, IM_COL32(red, green, blue, alpha));
                const bool keyed = k != keys.end();
                dl->AddRect(lo, hi, keyed ? key : IM_COL32(0, 0, 0, 160), 0.0f, 0,
                            keyed ? 2.0f : 1.0f);
                const float light =
                    (0.299f * red + 0.587f * green + 0.114f * blue) * alpha / 255.0f;
                col32 = light > 128.0f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
                inset = 3.0f;
            }
            cellText(a + inset, z - inset, y, col32, s);
        }
    }

    // The columns' rules
    const float gridBottom = y2 + rowH * static_cast<float>(std::min(visible, nrows - first));
    for (size_t i = 1; i < xs.size(); ++i)
        dl->AddLine(ImVec2(xs[i], y0), ImVec2(xs[i], gridBottom), border);
    ImGui::Dummy(ImVec2(tableRight - origin.x, gridBottom - origin.y));
}

// The frame's taps, asked for each frame: a restart of the machine turns the DMA record off
void ExocetVideoComposer::TakeFrame()
{
    AmigaEmulator* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu)
        return;
    emu->SetAudioTraceForCapture(true);
    emu->ArmDebugDmaForCapture();
    TakeDiskTransfers(*emu);
    TakeAudio(*emu);
    TakeDmaRecord(*emu);
}

// The floppy transfers the frame started. The core's log is a ring of the latest, numbered;
// the ones already taken are skipped by number, and a ring cleared (a disk change, a reset)
// starts the numbers again.
void ExocetVideoComposer::TakeDiskTransfers(AmigaEmulator& emu)
{
    for (auto& drive : m_heat)
        for (auto& side : drive)
            for (auto& track : side)
                track.heat *= kHeatFade;

    const int count = emu.GetDiskDmaEventCount();
    const AmigaEmulator::DiskDmaEvent* events = emu.GetDiskDmaEvents();
    if (count <= 0 || !events) {
        if (m_seqSeeded)
            m_nextSeq = 0;
        m_seqSeeded = true;
        return;
    }
    uint32_t top = 0;
    for (int i = 0; i < count; ++i)
        top = std::max(top, events[i].seq);
    // What was there before the film is not part of it
    if (!m_seqSeeded) {
        m_seqSeeded = true;
        m_nextSeq = top + 1;
        return;
    }
    if (top + 1 < m_nextSeq)
        m_nextSeq = 0;

    std::vector<const AmigaEmulator::DiskDmaEvent*> fresh;
    for (int i = 0; i < count; ++i)
        if (events[i].seq >= m_nextSeq)
            fresh.push_back(&events[i]);
    std::sort(fresh.begin(), fresh.end(),
              [](const auto* a, const auto* b) { return a->seq < b->seq; });
    const uint64_t frame = emu.GetTotalFrameCount();
    for (const auto* e : fresh) {
        Transfer t;
        t.frame = frame;
        t.drive = e->drive;
        t.cyl = e->cyl;
        t.side = e->side & 1;
        t.write = e->direction == 3;
        t.fromCopper = e->fromCopper != 0;
        t.aborted = e->aborted != 0;
        t.words = e->words;
        t.sync = e->dsksync;
        t.address = e->dskptStart;
        if (t.drive >= 0 && t.drive < 4) {
            TrackHeat& h = m_heat[t.drive][t.side][std::clamp(t.cyl, 0, kCylinders - 1)];
            h.heat = 1.0f;
            h.write = t.write;
            m_lastSide[t.drive] = t.side;
        }
        m_transfers.push_back(t);
    }
    while (m_transfers.size() > kTransfersKept)
        m_transfers.pop_front();
    m_nextSeq = top + 1;
}

// Each voice's samples from the frame, after its volume, as the Audio window's scopes take them
void ExocetVideoComposer::TakeAudio(AmigaEmulator& emu)
{
    for (int ch = 0; ch < 4; ++ch) {
        Voice& v = m_voices[ch];
        int count = 0;
        const auto* events = emu.GetAudioTrace(ch, count);
        if (!events || count <= 0) {
            for (int i = 0; i < kSilencePush; ++i) {
                v.samples[v.writePos] = 0.0f;
                v.writePos = (v.writePos + 1) % kScopeSamples;
            }
            v.dmaOn = false;
            v.peak *= 0.9f;
            continue;
        }
        float peak = 0.0f;
        for (int i = 0; i < count; ++i) {
            const float vol = std::min<float>(events[i].vol, 64.0f) / 64.0f;
            const float x =
                std::clamp(static_cast<float>(events[i].sample) / 128.0f * vol, -1.0f, 1.0f);
            v.samples[v.writePos] = x;
            v.writePos = (v.writePos + 1) % kScopeSamples;
            peak = std::max(peak, std::fabs(x));
        }
        const auto& last = events[count - 1];
        v.per = last.per;
        v.vol = std::min<uint16_t>(last.vol, 64);
        v.dmaOn = (last.flags & 2) != 0;
        v.peak = std::max(peak, v.peak * 0.9f);
    }
}

// The frame's DMA record as a picture, a slot a pixel - a line a row, a colour clock a column -
// and the slots counted by kind
void ExocetVideoComposer::TakeDmaRecord(AmigaEmulator& emu)
{
    AmigaEmulator::DmaSlotRecord* records = nullptr;
    int hposCount = 0, vposCount = 0;
    m_dmaValid = emu.GetDmaRecord(&records, hposCount, vposCount) && records && hposCount > 0;
    if (!m_dmaValid)
        return;
    const int columns =
        std::min<int>(hposCount, static_cast<int>(core::constants::beam::CCKPerLine));
    const int lines = core::copraster::DeriveFrameHeight(records, hposCount, vposCount, 313);
    m_dmaCounts.fill(0);
    m_dmaPixels.resize(static_cast<size_t>(columns) * lines);
    for (int v = 0; v < lines; ++v)
        for (int h = 0; h < columns; ++h) {
            const int type = records[static_cast<size_t>(v) * hposCount + h].type;
            const int kind = type > 0 && type < static_cast<int>(m_dmaCounts.size()) ? type : 0;
            ++m_dmaCounts[kind];
            // IM_COL32 holds red in its low byte: the bytes are RGBA in memory, as GL wants them
            m_dmaPixels[static_cast<size_t>(v) * columns + h] = DmaTypeColor(kind);
        }
    if (!m_dmaTexture) {
        glGenTextures(1, &m_dmaTexture);
        glBindTexture(GL_TEXTURE_2D, m_dmaTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, m_dmaTexture);
    if (columns != m_dmaColumns || lines != m_dmaLines) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, columns, lines, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     m_dmaPixels.data());
        m_dmaColumns = columns;
        m_dmaLines = lines;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, columns, lines, GL_RGBA, GL_UNSIGNED_BYTE,
                        m_dmaPixels.data());
    }
}

// The floppy drives: a drive a row - its head's cylinder in the status bar's readout, which is
// also the lamp, what it holds, and its tracks with the ones read or written lately glowing -
// then the transfers, newest first
void ExocetVideoComposer::DrawDrives()
{
    AmigaEmulator* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (!emu) {
        ImGui::TextDisabled("No machine");
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const ImU32 ghost = IM_COL32(255, 255, 255, 18);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

    for (int d = 0; d < 4; ++d) {
        const auto& ds = m_emulatorView->GetDriveState(d);
        if (d > 0 && !ds.enabled)
            continue;
        const bool motor = emu->IsDriveActive(d);
        const bool writing = emu->IsDriveWriting(d);
        const int cyl = emu->GetCurrentTrack(d);
        const ImVec4 lamp = !motor ? kLampIdle : writing ? kLampWrite : kLampRead;

        // The readout, two lines tall, then the name and state over what it holds
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = at.x + ImGui::GetContentRegionAvail().x;
        const float segH = lineH * 2.0f + st.ItemSpacing.y;
        DrawSevenSegment(dl, at, segH, cyl, ImGui::GetColorU32(lamp), ghost);
        const float textX = at.x + SevenSegmentWidth(segH) + st.ItemSpacing.x * 2.0f;
        const std::string state =
            !motor ? std::string("idle")
                   : fmt::format("{} cylinder {}, side {}", writing ? "writing" : "reading", cyl,
                                 m_lastSide[d]);
        dl->AddText(ImVec2(textX, at.y), text, fmt::format("DF{}", d).c_str());
        dl->AddText(ImVec2(textX + ImGui::CalcTextSize("DF0  ").x, at.y),
                    motor ? ImGui::GetColorU32(lamp) : dim, state.c_str());
        const std::string holds = ds.isBridged ? fmt::format("live drive, {}", ds.bridgeHardware)
                                  : ds.hasDisk ? ds.diskName
                                               : std::string("empty");
        const ImVec4 clip(textX, at.y, right, at.y + segH);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(textX, at.y + lineH + st.ItemSpacing.y), dim, holds.c_str(), nullptr,
                    0.0f, &clip);
        ImGui::Dummy(ImVec2(1.0f, segH));

        // The tracks: a cell a cylinder, a row a side, the head's column outlined
        const ImVec2 map = ImGui::GetCursorScreenPos();
        const float sideW = ImGui::CalcTextSize("0 ").x;
        const float mapX = map.x + sideW;
        const float cellW = (right - mapX) / static_cast<float>(kCylinders);
        const float cellH = std::floor(lineH * 0.7f);
        for (int side = 0; side < 2; ++side) {
            const float y = map.y + static_cast<float>(side) * (cellH + 2.0f);
            dl->AddText(ImVec2(map.x, y + (cellH - lineH) * 0.5f), dim, side ? "1" : "0");
            for (int c = 0; c < kCylinders; ++c) {
                const TrackHeat& h = m_heat[d][side][c];
                const ImVec4 hot = h.write ? kLampWrite : kLampRead;
                const float k = std::clamp(h.heat, 0.0f, 1.0f);
                const ImVec4 col(0.16f + (hot.x - 0.16f) * k, 0.18f + (hot.y - 0.18f) * k,
                                 0.16f + (hot.z - 0.16f) * k, 1.0f);
                const float x0 = mapX + cellW * static_cast<float>(c);
                dl->AddRectFilled(ImVec2(x0, y),
                                  ImVec2(x0 + std::max(1.0f, cellW - 2.0f), y + cellH),
                                  ImGui::GetColorU32(col));
            }
        }
        const float hx = mapX + cellW * static_cast<float>(std::clamp(cyl, 0, kCylinders - 1));
        dl->AddRect(
            ImVec2(hx - 2.0f, map.y - 2.0f), ImVec2(hx + cellW, map.y + cellH * 2.0f + 4.0f),
            motor ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 90), 0.0f, 0, 2.0f);
        ImGui::Dummy(ImVec2(1.0f, cellH * 2.0f + 4.0f));
        ImGui::Spacing();
    }

    // The transfers, newest first, as many as fit, in columns
    ImGui::Separator();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float bottom = origin.y + ImGui::GetContentRegionAvail().y;
    const float rowH = lineH + st.ItemSpacing.y;
    // Each column's width, the frame's first
    const float widths[] = {
        ImGui::CalcTextSize("0000000  ").x,      ImGui::CalcTextSize("DF0  ").x,
        ImGui::CalcTextSize("write  ").x,        ImGui::CalcTextSize("cyl 00 side 0  ").x,
        ImGui::CalcTextSize("00,000 words  ").x, ImGui::CalcTextSize("$4489  ").x,
    };
    float xs[7];
    xs[0] = origin.x;
    for (int i = 0; i < 6; ++i)
        xs[i + 1] = xs[i] + widths[i];
    auto put = [&](int col, float y, ImU32 c, const std::string& s) {
        dl->AddText(ImVec2(xs[col], y), c, s.c_str());
    };
    float y = origin.y;
    put(0, y, dim, "Frame");
    put(1, y, dim, "Drive");
    put(3, y, dim, "Track");
    put(4, y, dim, "Length");
    put(5, y, dim, "Sync");
    put(6, y, dim, "To");
    y += rowH;
    if (m_transfers.empty())
        put(0, y, dim, "No disk transfers since the film started");
    const uint64_t now = emu->GetTotalFrameCount();
    for (auto it = m_transfers.rbegin(); it != m_transfers.rend() && y + rowH <= bottom; ++it) {
        const Transfer& t = *it;
        // The frame's own transfers lit, the earlier ones plain
        const ImU32 c =
            t.frame + 1 >= now ? ImGui::GetColorU32(t.write ? kLampWrite : kLampRead) : text;
        put(0, y, dim, std::to_string(t.frame));
        put(1, y, c, t.drive < 4 ? fmt::format("DF{}", t.drive) : std::string("--"));
        put(2, y, c, t.write ? "write" : "read");
        put(3, y, c, fmt::format("cyl {} side {}", t.cyl, t.side));
        put(4, y, text, fmt::format("{} words", Thousands(t.words)));
        put(5, y, dim, fmt::format("${:04X}", t.sync));
        put(6, y, dim,
            fmt::format("${:06X}{}{}", t.address, t.fromCopper ? "  copper" : "",
                        t.aborted ? "  aborted" : ""));
        y += rowH;
    }
    ImGui::Dummy(ImVec2(1.0f, std::max(0.0f, y - origin.y)));
}

// Paula's four voices: a row each, what it plays at and its last samples as a trace
void ExocetVideoComposer::DrawPaula()
{
    AmigaEmulator* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (emu && !emu->HasAudioDebug()) {
        ImGui::TextDisabled("This core cannot show Paula's voices (it predates the audio tap)");
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = st.ItemSpacing.y;
    const float rowH = (avail.y - gap * 3.0f) / 4.0f;
    const float labelW = ImGui::CalcTextSize("per 0000  vol 64  ").x;
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    std::array<ImVec2, kScopeSamples> points;

    for (int ch = 0; ch < 4; ++ch) {
        const Voice& v = m_voices[ch];
        const float y = origin.y + (rowH + gap) * static_cast<float>(ch);
        const ImU32 colour = v.dmaOn ? kVoiceColour[ch] : IM_COL32(110, 110, 110, 255);
        dl->AddText(ImVec2(origin.x, y), colour,
                    fmt::format("Ch {}  {}", ch + 1,
                                v.dmaOn ? core::PaulaPeriodToNote(v.per) : std::string("off"))
                        .c_str());
        if (v.dmaOn)
            dl->AddText(ImVec2(origin.x, y + lineH), dim,
                        fmt::format("per {}  vol {}", v.per, v.vol).c_str());
        // the level, under the words
        const float meterY = y + lineH * 2.0f + 4.0f;
        const float meterW = labelW - st.ItemSpacing.x * 2.0f;
        if (meterY + 6.0f <= y + rowH) {
            dl->AddRectFilled(ImVec2(origin.x, meterY), ImVec2(origin.x + meterW, meterY + 6.0f),
                              IM_COL32(30, 30, 34, 255));
            dl->AddRectFilled(
                ImVec2(origin.x, meterY),
                ImVec2(origin.x + meterW * std::clamp(v.peak, 0.0f, 1.0f), meterY + 6.0f), colour);
        }

        const ImVec2 p0(origin.x + labelW, y);
        const ImVec2 p1(origin.x + avail.x, y + rowH);
        dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 22, 255));
        dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255));
        const float mid = (p0.y + p1.y) * 0.5f;
        const float half = (p1.y - p0.y) * 0.5f - 3.0f;
        dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), IM_COL32(60, 60, 70, 255));
        const float step = (p1.x - p0.x - 4.0f) / static_cast<float>(kScopeSamples - 1);
        for (int i = 0; i < kScopeSamples; ++i) {
            const float s = v.samples[(v.writePos + i) % kScopeSamples];
            points[i] = ImVec2(p0.x + 2.0f + step * static_cast<float>(i), mid - s * half);
        }
        dl->PushClipRect(p0, p1, true);
        dl->AddPolyline(points.data(), kScopeSamples, colour, ImDrawFlags_None, 2.0f);
        dl->PopClipRect();
    }
    ImGui::Dummy(avail);
}

// The frame's DMA slots: a colour clock a column and a line a row, coloured by what had the
// bus, beside how many slots each took
void ExocetVideoComposer::DrawDma()
{
    AmigaEmulator* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    if (emu && !emu->HasDmaDebug()) {
        ImGui::TextDisabled("This core keeps no DMA record");
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float swatch = lineH * 0.8f;
    const float legendW = swatch + ImGui::CalcTextSize("  Bitplane  00,000  100.0%").x;

    // A colour clock is two low-resolution pixels wide and a line one tall: the map at the
    // shape of the screen
    const int columns =
        m_dmaValid ? m_dmaColumns : static_cast<int>(core::constants::beam::CCKPerLine);
    const int lines = m_dmaValid ? m_dmaLines : 313;
    const float shape = static_cast<float>(columns) * 2.0f / static_cast<float>(lines);
    const float roomW = std::max(1.0f, avail.x - legendW - st.ItemSpacing.x * 2.0f);
    float mapH = avail.y;
    float mapW = mapH * shape;
    if (mapW > roomW) {
        mapW = roomW;
        mapH = mapW / shape;
    }
    const ImVec2 m0 = origin;
    const ImVec2 m1(origin.x + mapW, origin.y + mapH);
    if (m_dmaValid && m_dmaTexture) {
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
        ImGui::Image(static_cast<ImTextureID>(m_dmaTexture), ImVec2(mapW, mapH));
        dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
    } else {
        dl->AddRectFilled(m0, m1, kDmaColorIdle);
        const char* none = "No DMA record yet";
        const ImVec2 sz = ImGui::CalcTextSize(none);
        dl->AddText(ImVec2(m0.x + (mapW - sz.x) * 0.5f, m0.y + (mapH - sz.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), none);
    }

    // The legend: each kind's slots, and its share of the frame's
    const float lx = m1.x + st.ItemSpacing.x * 2.0f;
    const uint32_t total = static_cast<uint32_t>(columns) * static_cast<uint32_t>(lines);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float countX = lx + swatch + ImGui::CalcTextSize("  Bitplane  ").x;
    const float shareX = countX + ImGui::CalcTextSize("00,000  ").x;
    float y = origin.y;
    auto row = [&](int kind) {
        const uint32_t n = m_dmaValid ? m_dmaCounts[kind] : 0;
        const float sy = y + (lineH - swatch) * 0.5f;
        dl->AddRectFilled(ImVec2(lx, sy), ImVec2(lx + swatch, sy + swatch), DmaTypeColor(kind));
        dl->AddRect(ImVec2(lx, sy), ImVec2(lx + swatch, sy + swatch), IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(lx + swatch + st.ItemSpacing.x, y), n ? text : dim, DmaTypeLabel(kind));
        dl->AddText(ImVec2(countX, y), n ? text : dim, Thousands(n).c_str());
        dl->AddText(ImVec2(shareX, y), dim,
                    fmt::format("{:.1f}%", total ? 100.0 * n / total : 0.0).c_str());
        y += lineH + st.ItemSpacing.y;
    };
    for (int kind : kDmaKinds)
        row(kind);
    row(0);
    y += st.ItemSpacing.y;
    dl->AddText(ImVec2(lx, y), dim,
                fmt::format("{} lines, {} slots a line", lines, columns).c_str());
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(avail);
}

} // namespace adm::ui
