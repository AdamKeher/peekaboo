#include "AudioChannelsView.h"
#include "../TableLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../AddressLink.h"
#include "../DmaColors.h"
#include "../HyperlinkText.h"
#include "../IconsFontAwesome6.h"
#include "EmulatorView.h"

namespace adm::ui {

namespace {

// DMACON / INTREQ / ADKCON bits this view reads. Named here rather than inline so the register
// table reads as hardware documentation instead of magic numbers.
// Read from DMACONR (0x002), never DMACON (0x096): the latter is a raw write shadow holding the
// last SET/CLR word the CPU wrote, so a clear looks identical to a set. DMACONR is live state.
constexpr uint16_t kDmaconDmaEn = 0x0200;
constexpr uint16_t kIntAud0 = 0x0080;          // AUD0 interrupt; channel n is kIntAud0 << n
constexpr uint16_t kAdkconAttachVol0 = 0x0001; // USE0V1: channel n modulates n+1's volume
constexpr uint16_t kAdkconAttachPer0 = 0x0010; // USE0P1: ... its period

// Distinct colour per voice. Deliberately not all kDmaColorAudio: four yellow traces in one grid
// are unreadable, and these match the order the scrubber's DMA rows use.
constexpr ImU32 kChannelColor[4] = {
    IM_COL32(220, 200, 60, 255),  // yellow
    IM_COL32(80, 200, 200, 255),  // cyan
    IM_COL32(200, 120, 200, 255), // magenta
    IM_COL32(120, 200, 120, 255), // green
};

constexpr ImU32 kMutedColor = IM_COL32(110, 110, 110, 255);

// AUDxVOL is write-only, so every register-side read here is the raw last-written word from the
// write shadow. Paula only looks at bits 0-6 and saturates at 64, so decode it the way the
// hardware does rather than printing a value no volume can be (a player that pokes AUDxVOL and
// AUDxDAT with one move.l leaves the sample pair sitting in the high bits).
uint16_t PaulaVolume(uint16_t raw)
{
    const uint16_t v = raw & 0x7F;
    return v > 64 ? 64 : v;
}

ImU32 WithAlpha(ImU32 col, int alpha)
{
    return (col & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

const char* FilterModelName(int model)
{
    switch (model) {
        case 1:
            return "A500";
        case 2:
            return "A1200";
        case 3:
            return "A500 fixed";
        default:
            return "none";
    }
}

} // namespace

AudioChannelsView::AudioChannelsView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

adm::AmigaEmulator* AudioChannelsView::Emulator() const
{
    return m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
}

void AudioChannelsView::Clear()
{
    for (auto& s : m_scopes)
        s = ChannelScope{};
    m_lastHistoryFrame = UINT64_MAX;
    m_lastRegsFrame = UINT64_MAX;
    m_regsValid = false;
}

void AudioChannelsView::SetUserPreferences(core::UserPreferences* preferences)
{
    m_userPreferences = preferences;
    LoadPreferences();
}

void AudioChannelsView::LoadPreferences()
{
    if (!m_userPreferences || m_prefsLoaded)
        return;
    const auto& a = m_userPreferences->Get().audio;
    m_channelMask = static_cast<uint8_t>(a.channelMask & 0x0F);
    m_filterMode = a.filterMode;
    m_filterModel = a.filterModel;
    m_timeBaseSamples = std::clamp(a.timeBaseSamples, 64, kScopeHistory);
    m_yZoom = std::clamp(a.yZoom, 0.25f, 8.0f);
    m_showRegisters = a.showRegisters;
    m_preSoloMask = m_channelMask;
    m_prefsLoaded = true;
}

void AudioChannelsView::SavePreferences()
{
    if (!m_userPreferences)
        return;
    core::UserPreferences::Preferences::AudioPrefs a;
    a.channelMask = m_channelMask;
    a.filterMode = m_filterMode;
    a.filterModel = m_filterModel;
    a.timeBaseSamples = m_timeBaseSamples;
    a.yZoom = m_yZoom;
    a.showRegisters = m_showRegisters;
    if (!(m_userPreferences->Get().audio == a))
        m_userPreferences->SetAudioSettings(a);
}

void AudioChannelsView::SetCaptureActive(bool active)
{
    if (m_captureActive == active)
        return;
    m_captureActive = active;
    if (auto* emu = Emulator())
        emu->SetAudioTraceEnabled(active);
    if (active) {
        // The mask lives in the core and resets to "all audible" with the machine, so re-assert
        // whatever the user last chose whenever the window comes back.
        ApplyChannelMask();
        ApplyFilterSettings();
    } else {
        Clear();
    }
}

void AudioChannelsView::ToggleChannel(int channel)
{
    m_channelMask = static_cast<uint8_t>(m_channelMask ^ (1u << channel));
    m_soloChannel = -1; // a manual edit ends solo mode
    ApplyChannelMask();
    SavePreferences();
}

void AudioChannelsView::SoloChannel(int channel)
{
    if (m_soloChannel == channel) {
        // Soloing the same channel again restores whatever was audible before solo started.
        m_channelMask = m_preSoloMask;
        m_soloChannel = -1;
    } else {
        if (m_soloChannel < 0)
            m_preSoloMask = m_channelMask;
        m_channelMask = static_cast<uint8_t>(1u << channel);
        m_soloChannel = channel;
    }
    ApplyChannelMask();
    SavePreferences();
}

void AudioChannelsView::ApplyChannelMask()
{
    if (auto* emu = Emulator())
        emu->SetAudioChannelMask(m_channelMask);
}

void AudioChannelsView::ApplyFilterSettings()
{
    auto* emu = Emulator();
    if (!emu)
        return;
    // PuAE already exposes both of these as core options, and routing through them means
    // set_audio() recomputes the filter coefficients properly. Poking the statics directly
    // would skip that.
    static const char* kModes[3] = {"off", "emulated", "on"};
    static const char* kModels[3] = {"auto", "standard", "enhanced"};
    emu->SetUserVariable("puae_sound_filter", kModes[std::clamp(m_filterMode, 0, 2)]);
    emu->SetUserVariable("puae_sound_filter_type", kModels[std::clamp(m_filterModel, 0, 2)]);
}

void AudioChannelsView::EnsureFrameData()
{
    auto* emu = Emulator();
    if (!emu)
        return;

    const uint64_t frame = m_emulatorView->GetEmulatorFrameCount();

    if (frame != m_lastRegsFrame) {
        m_regsValid = emu->GetCustomRegisters(m_regs);
        m_lastRegsFrame = frame;
    }

    // Re-assert the mask if the core disagrees — a reset or a fresh disk silently restores
    // audio_channel_mask to "all audible" underneath us.
    if (m_captureActive && emu->HasAudioDebug() && emu->GetAudioChannelMask() != m_channelMask)
        ApplyChannelMask();

    // Only advance the scope on a freshly emulated frame. While paused the frame counter is
    // static, so the traces hold instead of scrolling duplicated samples off the left edge.
    if (!m_frozen && frame != m_lastHistoryFrame) {
        PushFrame();
        m_lastHistoryFrame = frame;
    }
}

void AudioChannelsView::PushFrame()
{
    auto* emu = Emulator();
    if (!emu)
        return;

    for (int ch = 0; ch < kChannels; ++ch) {
        ChannelScope& sc = m_scopes[ch];
        int count = 0;
        const auto* events = emu->GetAudioTrace(ch, count);

        sc.lastFrameCount = count;
        sc.framePeak = 0.0f;
        sc.frameRms = 0.0f;

        if (!events || count <= 0) {
            // Silent voice: scroll flat rather than freezing on whatever it played last.
            for (int i = 0; i < kSilencePush; ++i) {
                sc.samples[sc.writePos] = 0.0f;
                sc.writePos = (sc.writePos + 1) % kScopeHistory;
            }
            sc.valid = std::min(sc.valid + kSilencePush, kScopeHistory);
            // No samples is not the same as no DMA: a long period can straddle a frame, and the
            // scrubber's CCK window can drop every event inside it. Fall back to DMACONR so the
            // header agrees with the register table instead of claiming the voice is off.
            sc.dmaOn = m_regsValid && (m_regs.dmaconr & kDmaconDmaEn) != 0 &&
                       (m_regs.dmaconr & (1u << ch)) != 0;
            // Period/volume come from the trace, so with no events they would read 0 and the
            // header would contradict the register table directly below it.
            if (m_regsValid) {
                sc.per = m_regs.aud[ch].ac_per;
                sc.vol = PaulaVolume(m_regs.aud[ch].ac_vol);
            }
            sc.peak *= kPeakDecayRate;
            continue;
        }

        double sumSq = 0.0;
        for (int i = 0; i < count; ++i) {
            const auto& ev = events[i];
            const float vol = std::min<float>(ev.vol, 64.0f) / 64.0f;
            const float v = std::clamp((static_cast<float>(ev.sample) / 128.0f) * vol, -1.0f, 1.0f);
            sc.samples[sc.writePos] = v;
            sc.writePos = (sc.writePos + 1) % kScopeHistory;
            const float mag = std::fabs(v);
            if (mag > sc.framePeak)
                sc.framePeak = mag;
            sumSq += static_cast<double>(v) * v;
        }
        sc.valid = std::min(sc.valid + count, kScopeHistory);
        sc.frameRms = static_cast<float>(std::sqrt(sumSq / count));

        const auto& last = events[count - 1];
        sc.per = last.per;
        sc.vol = last.vol;
        sc.dmaOn = (last.flags & 2) != 0;

        // Peak-hold with decay, so a transient stays readable for more than one 20 ms frame.
        sc.peak = std::max(sc.framePeak, sc.peak * kPeakDecayRate);
    }
}

// ---- toolbar ---------------------------------------------------------------------------------

void AudioChannelsView::DrawToolbar()
{
    auto* emu = Emulator();
    const bool hasTap = emu && emu->HasAudioDebug();
    bool maskChanged = false;
    bool prefsChanged = false;

    ImGui::TextUnformatted("Channels:");
    for (int ch = 0; ch < kChannels; ++ch) {
        ImGui::SameLine();
        ImGui::PushID(ch);
        bool on = (m_channelMask & (1u << ch)) != 0;
        char label[16];
        std::snprintf(label, sizeof(label), "%d", ch);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kChannelColor[ch]));
        if (ImGui::Checkbox(label, &on))
            ToggleChannel(ch);
        ImGui::PopStyleColor();

        ImGui::SameLine(0.0f, 2.0f);
        const bool soloed = (m_soloChannel == ch);
        if (soloed)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton("S"))
            SoloChannel(ch);
        if (soloed)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Solo channel %d (click again to restore)", ch);
        ImGui::PopID();
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("All")) {
        m_channelMask = 0x0F;
        m_soloChannel = -1;
        maskChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) {
        m_channelMask = 0x00;
        m_soloChannel = -1;
        maskChanged = true;
    }

    if (!hasTap) {
        ImGui::SameLine();
        ImGui::TextDisabled("(mute needs a newer core)");
    }

    // --- second row: filter + master mute ---
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Low-pass:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    const char* kModeLabels[3] = {"Off", "Emulated (LED)", "On"};
    if (ImGui::Combo("##filtermode", &m_filterMode, kModeLabels, 3)) {
        ApplyFilterSettings();
        prefsChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: filter bypassed.\nEmulated: follows the power LED, as on real "
                          "hardware.\nOn: filter always in circuit.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    const char* kModelLabels[3] = {"Auto", "A500", "A1200"};
    if (ImGui::Combo("##filtermodel", &m_filterModel, kModelLabels, 3)) {
        ApplyFilterSettings();
        prefsChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filter model. Auto picks A1200 for AGA machines, A500 otherwise.");

    // Live readback: in Emulated mode the filter follows the power LED, so what is actually in
    // circuit is not what the combo says.
    if (emu) {
        const auto fs = emu->GetAudioFilterState();
        ImGui::SameLine();
        if (fs.ledFilterOn)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "LED filter ON");
        else
            ImGui::TextDisabled("LED filter off");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Live filter state read from the core.\nModel in use: %s",
                              FilterModelName(fs.model));
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (m_emulatorView) {
        // Deliberately EmulatorView's mute, not AmigaEmulator's. EmulatorView owns the user's
        // intent and pushes down (turbo || muted), so reading the emulator directly would show
        // "muted" during Turbo and would be clobbered the next time either one changes. This is
        // the same call the emulator toolbar's speaker button makes.
        bool muted = m_emulatorView->IsMuted();
        if (ImGui::Checkbox(muted ? ICON_FA_VOLUME_XMARK " Mute" : ICON_FA_VOLUME_HIGH " Mute",
                            &muted))
            m_emulatorView->SetMuted(muted);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Host output mute, shared with the emulator toolbar.\n"
                              "Paula keeps running - the scopes stay live.");
    }

    // --- third row: scope controls ---
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Window:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderInt("##timebase", &m_timeBaseSamples, 64, kScopeHistory, "%d samples",
                         ImGuiSliderFlags_Logarithmic))
        prefsChanged = true;
    ImGui::SameLine();
    ImGui::TextUnformatted("Y:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::SliderFloat("##yzoom", &m_yZoom, 0.25f, 8.0f, "x%.2f", ImGuiSliderFlags_Logarithmic))
        prefsChanged = true;
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &m_frozen);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stop scrolling the scopes without pausing the emulator.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Registers", &m_showRegisters))
        prefsChanged = true;

    if (maskChanged) {
        ApplyChannelMask();
        prefsChanged = true;
    }
    if (prefsChanged)
        SavePreferences();
}

// ---- scopes ----------------------------------------------------------------------------------

AudioChannelsView::ScopeAction AudioChannelsView::DrawScope(int channel, ImVec2 pos, ImVec2 size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ChannelScope& sc = m_scopes[channel];
    const bool audible = (m_channelMask & (1u << channel)) != 0;
    const ImU32 traceColor = audible ? kChannelColor[channel] : kMutedColor;

    // Frame + background
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(18, 18, 22, 255));
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(70, 70, 80, 255));

    const float headerH = ImGui::GetTextLineHeight() + 4.0f;
    const float vuW = 10.0f;
    const float plotX = pos.x + 4.0f;
    const float plotY = pos.y + headerH;
    const float plotW = std::max(8.0f, size.x - 8.0f - vuW - 4.0f);
    const float plotH = std::max(8.0f, size.y - headerH - 4.0f);
    const float midY = plotY + plotH * 0.5f;

    // Zero line + half-scale guides
    dl->AddLine(ImVec2(plotX, midY), ImVec2(plotX + plotW, midY), IM_COL32(60, 60, 70, 255));
    for (int s = -1; s <= 1; s += 2) {
        const float y = midY + s * plotH * 0.25f;
        dl->AddLine(ImVec2(plotX, y), ImVec2(plotX + plotW, y), IM_COL32(38, 38, 46, 255));
    }

    // Header: channel number, DMA pill, period/volume
    char header[96];
    std::snprintf(header, sizeof(header), "CH%d", channel);
    dl->AddText(ImVec2(pos.x + 5.0f, pos.y + 2.0f), traceColor, header);

    const float infoX = pos.x + 40.0f;
    if (!sc.dmaOn) {
        dl->AddText(ImVec2(infoX, pos.y + 2.0f), IM_COL32(140, 140, 150, 255), "DMA off");
    } else {
        char info[96];
        const double hz = core::PaulaPeriodToHz(sc.per);
        std::snprintf(info, sizeof(info), "per %u (%.1f kHz %s)  vol %u", sc.per, hz / 1000.0,
                      core::PaulaPeriodToNote(sc.per).c_str(), sc.vol);
        dl->AddText(ImVec2(infoX, pos.y + 2.0f), IM_COL32(190, 190, 200, 255), info);
    }
    if (!audible) {
        const char* tag = "MUTED";
        const float w = ImGui::CalcTextSize(tag).x;
        dl->AddText(ImVec2(pos.x + size.x - w - 6.0f - vuW, pos.y + 2.0f),
                    IM_COL32(230, 120, 120, 255), tag);
    }

    // Waveform
    const int span = std::min(std::min(m_timeBaseSamples, kScopeHistory), sc.valid);
    if (span >= 2) {
        dl->PushClipRect(ImVec2(plotX, plotY), ImVec2(plotX + plotW, plotY + plotH), true);
        const float stepX = plotW / static_cast<float>(span - 1);
        const int start = (sc.writePos - span + kScopeHistory) % kScopeHistory;
        const ImU32 fillColor = WithAlpha(traceColor, 60);
        ImVec2 prev{};
        for (int i = 0; i < span; ++i) {
            const float v =
                std::clamp(sc.samples[(start + i) % kScopeHistory] * m_yZoom, -1.0f, 1.0f);
            const ImVec2 pt{plotX + stepX * i, midY - v * plotH * 0.5f};
            if (i > 0) {
                dl->AddQuadFilled(prev, pt, ImVec2(pt.x, midY), ImVec2(prev.x, midY), fillColor);
                dl->AddLine(prev, pt, traceColor, 1.0f);
            }
            prev = pt;
        }
        dl->PopClipRect();
    } else {
        const char* msg = "no samples";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(plotX + (plotW - ts.x) * 0.5f, midY - ts.y * 0.5f),
                    IM_COL32(90, 90, 100, 255), msg);
    }

    // VU meter down the right edge: filled bar = this frame's peak, tick = decaying peak-hold.
    const float vuX = pos.x + size.x - vuW - 3.0f;
    const ImVec2 vuTop{vuX, plotY};
    const ImVec2 vuBot{vuX + vuW, plotY + plotH};
    dl->AddRectFilled(vuTop, vuBot, IM_COL32(28, 28, 34, 255));
    const float level = std::clamp(sc.framePeak, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(vuX, vuBot.y - plotH * level), vuBot, WithAlpha(traceColor, 200));
    const float hold = std::clamp(sc.peak, 0.0f, 1.0f);
    if (hold > 0.001f) {
        const float hy = vuBot.y - plotH * hold;
        dl->AddLine(ImVec2(vuX, hy), ImVec2(vuX + vuW, hy), IM_COL32(255, 255, 255, 200), 1.0f);
    }
    dl->AddRect(vuTop, vuBot, IM_COL32(70, 70, 80, 255));

    // Hit-test the whole cell: left-click mutes/unmutes the voice, right-click solos it.
    // Both buttons have to be registered explicitly or IsItemClicked() never sees the right one.
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(("##scope" + std::to_string(channel)).c_str(), size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    ScopeAction action = ScopeAction::None;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        action = ScopeAction::Toggle;
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        action = ScopeAction::Solo;

    if (ImGui::IsItemHovered()) {
        // Hover outline, so a cell reads as clickable rather than as a static plot.
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), WithAlpha(traceColor, 170));
        ImGui::SetTooltip("Channel %d - %s%s\n%d samples this frame\npeak %.3f   rms %.3f\n\n"
                          "Click to %s, right-click to %s",
                          channel, audible ? "audible" : "muted",
                          m_soloChannel == channel ? " (solo)" : "", sc.lastFrameCount,
                          sc.framePeak, sc.frameRms, audible ? "mute" : "unmute",
                          m_soloChannel == channel ? "unsolo" : "solo");
    }

    return action;
}

void AudioChannelsView::DrawScopeGrid()
{
    auto* emu = Emulator();
    if (emu && !emu->HasAudioDebug()) {
        ImGui::TextDisabled("Paula channel capture not available (core predates the audio tap).");
        ImGui::TextDisabled("Rebuild PuAE with scripts/build_puae.%s to enable the scopes.",
#ifdef _WIN32
                            "ps1"
#else
                            "sh"
#endif
        );
        ImGui::Spacing();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 4.0f;
    const float cellW = (avail.x - gap) * 0.5f;
    const float cellH = (avail.y - gap) * 0.5f;
    if (cellW < 40.0f || cellH < 40.0f)
        return;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    int actionChannel = -1;
    ScopeAction action = ScopeAction::None;
    for (int ch = 0; ch < kChannels; ++ch) {
        const float x = origin.x + (ch % 2) * (cellW + gap);
        const float y = origin.y + (ch / 2) * (cellH + gap);
        const ScopeAction a = DrawScope(ch, ImVec2(x, y), ImVec2(cellW, cellH));
        if (a != ScopeAction::None) {
            action = a;
            actionChannel = ch;
        }
    }
    // Applied after the grid so the mask cannot change mid-draw and desync the cells.
    if (actionChannel >= 0) {
        if (action == ScopeAction::Toggle)
            ToggleChannel(actionChannel);
        else if (action == ScopeAction::Solo)
            SoloChannel(actionChannel);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ---- register table --------------------------------------------------------------------------

void AudioChannelsView::DrawPtrLink(const char* idSuffix, uint32_t addr)
{
    if (addr == 0) {
        ImGui::TextDisabled("---");
        return;
    }
    if (m_addrCtx && m_addrCtx->valid()) {
        DrawAddressLink(idSuffix, addr, "$%06X", *m_addrCtx, AddressLinkHost::Window,
                        AddressDefault::Memory);
        return;
    }
    ImGui::Text("$%06X", addr);
}

void AudioChannelsView::DrawRegisterTable()
{
    if (!m_regsValid) {
        ImGui::TextDisabled("Custom registers unavailable.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  kAdjustableColumns | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("paula_channels", 12, flags))
        return;

    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("AUDxLC", ImGuiTableColumnFlags_WidthFixed, 66.0f);
    ImGui::TableSetupColumn("LEN", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("PER", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Hz", ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("VOL", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("DMA", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("INT", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Att", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("RMS", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableHeadersRow();

    const bool masterDma = (m_regs.dmaconr & kDmaconDmaEn) != 0;

    for (int ch = 0; ch < kChannels; ++ch) {
        const auto& a = m_regs.aud[ch];
        const uint32_t lc = core::DecodeAmigaPtr(a.ac_ptr);
        const uint16_t per = a.ac_per;
        const uint16_t vol = PaulaVolume(a.ac_vol);
        const bool dmaOn = masterDma && (m_regs.dmaconr & (1u << ch)) != 0;
        const uint16_t intBit = static_cast<uint16_t>(kIntAud0 << ch);
        const bool audible = (m_channelMask & (1u << ch)) != 0;

        ImGui::TableNextRow();
        ImGui::PushID(ch);

        ImGui::TableNextColumn();
        ImGui::TextColored(
            ImGui::ColorConvertU32ToFloat4(audible ? kChannelColor[ch] : kMutedColor), "%d", ch);

        ImGui::TableNextColumn();
        char suffix[24];
        std::snprintf(suffix, sizeof(suffix), "audlc%d", ch);
        DrawPtrLink(suffix, lc);

        ImGui::TableNextColumn();
        ImGui::Text("%u", a.ac_len);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%u words (%u bytes)", a.ac_len, a.ac_len * 2u);

        ImGui::TableNextColumn();
        ImGui::Text("%u", per);

        ImGui::TableNextColumn();
        const double hz = core::PaulaPeriodToHz(per);
        if (hz > 0.0)
            ImGui::Text("%.0f", hz);
        else
            ImGui::TextDisabled("--");

        ImGui::TableNextColumn();
        int cents = 0;
        const std::string note = core::PaulaPeriodToNote(per, &cents);
        ImGui::TextUnformatted(note.c_str());
        if (per >= 2 && ImGui::IsItemHovered())
            ImGui::SetTooltip("ProTracker note for this period (%+d cents).\nThe pitch you hear "
                              "also depends on the sample loaded.",
                              cents);

        ImGui::TableNextColumn();
        ImGui::Text("%u", vol);
        if (ImGui::IsItemHovered() && a.ac_vol != vol)
            ImGui::SetTooltip("AUDxVOL is write-only; this is the write shadow. Raw $%04X, "
                              "of which Paula uses bits 0-6 (max 64).",
                              a.ac_vol);

        ImGui::TableNextColumn();
        if (dmaOn)
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "on");
        else
            ImGui::TextDisabled("off");

        ImGui::TableNextColumn();
        {
            const bool pending = (m_regs.intreq & intBit) != 0;
            const bool enabled = (m_regs.intena & intBit) != 0;
            if (pending)
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "req");
            else if (enabled)
                ImGui::TextDisabled("ena");
            else
                ImGui::TextDisabled("--");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("INTREQ %s, INTENA %s (bit $%04X)", pending ? "set" : "clear",
                                  enabled ? "set" : "clear", intBit);
        }

        ImGui::TableNextColumn();
        {
            const bool attVol = (m_regs.adkcon & (kAdkconAttachVol0 << ch)) != 0;
            const bool attPer = (m_regs.adkcon & (kAdkconAttachPer0 << ch)) != 0;
            if (!attVol && !attPer) {
                ImGui::TextDisabled("--");
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.95f, 1.0f), "%s%s", attVol ? "V" : "",
                                   attPer ? "P" : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "ADKCON: channel %d modulates channel %d's %s", ch, (ch + 1) & 3,
                        attVol && attPer ? "volume and period" : (attVol ? "volume" : "period"));
            }
        }

        ImGui::TableNextColumn();
        ImGui::Text("%.3f", m_scopes[ch].framePeak);

        ImGui::TableNextColumn();
        ImGui::Text("%.3f", m_scopes[ch].frameRms);

        ImGui::PopID();
    }

    ImGui::EndTable();
}

// ---- status bar ------------------------------------------------------------------------------

void AudioChannelsView::DrawStatusBar()
{
    auto* emu = Emulator();
    int total = 0;
    for (const auto& s : m_scopes)
        total += s.lastFrameCount;

    if (emu && emu->HasAudioDebug())
        ImGui::Text("%d samples/frame", total);
    else
        ImGui::TextDisabled("no Paula tap");

    if (!m_onGetAudioStats)
        return;

    const core::AudioLogSnapshot st = m_onGetAudioStats();
    if (!st.available || !st.initialized) {
        ImGui::SameLine();
        ImGui::TextDisabled("| host audio not running");
        return;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    // Host-path health, so a glitch can be blamed on the right layer without leaving the window.
    const bool sick = st.underrunCallbacks > 0 || st.framesDroppedOverrun > 0;
    const ImVec4 col = sick ? ImVec4(0.95f, 0.65f, 0.35f, 1.0f) : ImVec4(0.55f, 0.55f, 0.6f, 1.0f);
    ImGui::TextColored(col, "ring %u/%u  underruns %llu  held %llu  dropped %llu", st.ringLevel,
                       st.ringSize, static_cast<unsigned long long>(st.underrunCallbacks),
                       static_cast<unsigned long long>(st.framesHeld),
                       static_cast<unsigned long long>(st.framesDroppedOverrun));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Host audio path (the same numbers the AUDIO console command prints).\n"
                          "Underruns and held frames mean the fault is downstream of Paula.");
}

// ---- snapshot --------------------------------------------------------------------------------

core::PaulaStateSnapshot AudioChannelsView::BuildSnapshot() const
{
    core::PaulaStateSnapshot snap;
    auto* emu = Emulator();
    if (!emu)
        return snap;

    core::Custom regs{};
    if (!emu->GetCustomRegisters(regs))
        return snap;

    snap.available = true;
    snap.traceAvailable = emu->HasAudioDebug();
    snap.masterDmaOn = (regs.dmaconr & kDmaconDmaEn) != 0;
    // What decides what is heard, not only what this window chose: a held silence (--silent)
    // keeps the host muted and the core's mask at 0 whatever the window says.
    snap.hostMuted = (m_emulatorView && m_emulatorView->IsMuted()) || emu->IsForceSilent();
    snap.channelMask = emu->IsForceSilent() ? emu->GetAudioChannelMask() : m_channelMask;

    const auto fs = emu->GetAudioFilterState();
    snap.ledFilterOn = fs.ledFilterOn;
    snap.filterModel = fs.model;

    for (int ch = 0; ch < kChannels; ++ch) {
        auto& c = snap.channels[ch];
        const auto& a = regs.aud[ch];
        c.index = ch;
        c.lc = core::DecodeAmigaPtr(a.ac_ptr);
        c.len = a.ac_len;
        c.per = a.ac_per;
        c.vol = PaulaVolume(a.ac_vol);
        c.hz = core::PaulaPeriodToHz(c.per);
        c.note = core::PaulaPeriodToNote(c.per);
        c.dmaOn = snap.masterDmaOn && (regs.dmaconr & (1u << ch)) != 0;
        c.audible = (snap.channelMask & (1u << ch)) != 0;
        const uint16_t intBit = static_cast<uint16_t>(kIntAud0 << ch);
        c.intPending = (regs.intreq & intBit) != 0;
        c.intEnabled = (regs.intena & intBit) != 0;
        c.attachVolume = (regs.adkcon & (kAdkconAttachVol0 << ch)) != 0;
        c.attachPeriod = (regs.adkcon & (kAdkconAttachPer0 << ch)) != 0;
        c.sampleCount = m_scopes[ch].lastFrameCount;
        c.peak = m_scopes[ch].framePeak;
        c.rms = m_scopes[ch].frameRms;
    }
    return snap;
}

// ---- main draw -------------------------------------------------------------------------------

void AudioChannelsView::Draw()
{
    LoadPreferences();

    auto* emu = Emulator();
    if (!emu || !emu->IsGameLoaded()) {
        ImGui::TextDisabled("No emulation running.");
        return;
    }

    EnsureFrameData();

    DrawToolbar();
    ImGui::Separator();

    const float statusH = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    float tableH = 0.0f;
    if (m_showRegisters)
        tableH = ImGui::GetTextLineHeightWithSpacing() * (kChannels + 1) + 12.0f;

    const float gridH = std::max(80.0f, ImGui::GetContentRegionAvail().y - tableH - statusH);
    if (ImGui::BeginChild("##scopes", ImVec2(0.0f, gridH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        DrawScopeGrid();
    }
    ImGui::EndChild();

    if (m_showRegisters) {
        ImGui::Spacing();
        DrawRegisterTable();
    }

    ImGui::Separator();
    DrawStatusBar();
}

} // namespace adm::ui
