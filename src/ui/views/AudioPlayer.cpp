#include "AudioPlayer.h"

#include "../../core/AudioEngine.h"
#include "../../core/AudioModule.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace adm {
namespace ui {

AudioPlayer::AudioPlayer(core::AudioEngine* engine) : m_engine(engine)
{
    m_waveformBuffer.resize(kWaveformSamples, 0.0f);
}

AudioPlayer::~AudioPlayer()
{
    // Stop playback if this player's module is currently playing
    if (m_engine && m_module) {
        m_engine->Stop();
    }
}

void AudioPlayer::SetModule(std::unique_ptr<core::AudioModule> module)
{
    // Stop any current playback
    if (m_engine) {
        m_engine->Stop();
    }

    m_module = std::move(module);

    // Set sample rate from engine
    if (m_module && m_engine) {
        m_module->SetSampleRate(m_engine->GetSampleRate());
    }

    // Cache sample names and channel info
    m_sampleNames.clear();
    m_channelNames.clear();
    m_numChannels = 0;

    if (m_module) {
        m_sampleNames = m_module->GetSampleNames();
        m_channelNames = m_module->GetChannelNames();
        m_numChannels = static_cast<int>(m_channelNames.size());
    }

    // Reset pattern view state
    m_lastScrolledRow = -1;
    m_followPlayback = true;
}

void AudioPlayer::SetTitle(const std::string& title) { m_title = title; }

void AudioPlayer::Open() { m_isOpen = true; }

void AudioPlayer::Draw()
{
    if (!m_isOpen) {
        return;
    }

    // ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);

    // Use ### naming convention to keep saved settings (size/pos) separate from title (which may
    // change)
    if (ImGui::Begin((m_title + "###AudioPlayer").c_str(), &m_isOpen)) {
        if (!m_module || !m_module->IsLoaded()) {
            ImGui::TextDisabled("No module loaded");
        } else {
            DrawModuleInfo();
            ImGui::Separator();
            DrawPlaybackControls();
            ImGui::Separator();
            DrawSeekBar();
            ImGui::Separator();

            // Tab bar for waveform, samples, and patterns
            if (ImGui::BeginTabBar("##AudioPlayerTabs")) {
                if (ImGui::BeginTabItem("Waveform")) {
                    DrawWaveform();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Samples")) {
                    DrawSamplesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Patterns")) {
                    DrawPatternsTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
    }
    ImGui::End();

    // Handle window close - stop playback
    if (!m_isOpen && m_engine) {
        m_engine->Stop();
    }
}

void AudioPlayer::DrawModuleInfo()
{
    if (!m_module) {
        return;
    }

    auto info = m_module->GetInfo();

    if (!info.title.empty()) {
        ImGui::Text("Title: %s", info.title.c_str());
    }
    if (!info.artist.empty()) {
        ImGui::Text("Artist: %s", info.artist.c_str());
    }
    if (!info.tracker.empty()) {
        ImGui::TextDisabled("Tracker: %s", info.tracker.c_str());
    }

    ImGui::Text("Duration: %s", FormatTime(info.durationSeconds).c_str());

    // Show module stats in a compact line
    ImGui::TextDisabled("%d ch, %d pat, %d smp", info.numChannels, info.numPatterns,
                        info.numSamples);
}

void AudioPlayer::DrawPlaybackControls()
{
    if (!m_module || !m_engine) {
        return;
    }

    bool isPlaying = m_engine->IsPlaying();
    bool isPaused = m_engine->IsPaused();

    // Play button
    if (!isPlaying || isPaused) {
        if (ImGui::Button("Play")) {
            if (isPaused) {
                m_engine->Resume();
            } else {
                m_engine->Play(m_module.get());
            }
        }
    } else {
        if (ImGui::Button("Pause")) {
            m_engine->Pause();
        }
    }

    ImGui::SameLine();

    // Stop button
    ImGui::BeginDisabled(!isPlaying && !isPaused);
    if (ImGui::Button("Stop")) {
        m_engine->Stop();
        m_module->SetPositionSeconds(0.0);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    // Restart button
    if (ImGui::Button("Restart")) {
        m_module->SetPositionSeconds(0.0);
        if (!isPlaying) {
            m_engine->Play(m_module.get());
        }
    }

    // Status indicator
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    if (isPlaying && !isPaused) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Playing");
    } else if (isPaused) {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Paused");
    } else {
        ImGui::TextDisabled("Stopped");
    }
}

void AudioPlayer::DrawSeekBar()
{
    if (!m_module || !m_engine) {
        return;
    }

    double position = m_module->GetPositionSeconds();
    double duration = m_module->GetDurationSeconds();

    if (duration <= 0.0) {
        return;
    }

    float progress = static_cast<float>(position / duration);

    // Time display
    std::string timeText = FormatTime(position) + " / " + FormatTime(duration);

    // Seek slider
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##seek", &progress, 0.0f, 1.0f, "")) {
        m_module->SetPositionSeconds(progress * duration);
    }

    // Handle drag start - pause audio to avoid glitches
    if (ImGui::IsItemActivated()) {
        m_wasPlayingBeforeSeek = m_engine->IsPlaying() && !m_engine->IsPaused();
        if (m_wasPlayingBeforeSeek) {
            m_engine->Pause();
        }
        m_isDraggingSeekBar = true;
    }

    // Handle drag end - resume if was playing before
    if (ImGui::IsItemDeactivated()) {
        if (m_wasPlayingBeforeSeek) {
            m_engine->Resume();
        }
        m_isDraggingSeekBar = false;
        m_wasPlayingBeforeSeek = false;
    }

    // Center the time text
    float textWidth = ImGui::CalcTextSize(timeText.c_str()).x;
    float windowWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (windowWidth - textWidth) * 0.5f);
    ImGui::TextUnformatted(timeText.c_str());
}

void AudioPlayer::DrawWaveform()
{
    if (!m_module) {
        return;
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();
    float height = std::max(availSize.y, 50.0f);
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Get data
    m_waveformBuffer = m_module->GetCurrentWaveform(kWaveformSamples);
    std::vector<float> vuLevels = m_module->GetChannelVULevels();

    int numChannels = static_cast<int>(vuLevels.size());
    if (numChannels == 0)
        return;

    // Layout
    float spacing = 2.0f;
    float channelWidth = (availSize.x - (numChannels - 1) * spacing) / numChannels;

    // Background colors
    const ImU32 colorGreen = IM_COL32(0, 200, 0, 180);
    const ImU32 colorOrange = IM_COL32(255, 140, 0, 180);
    const ImU32 colorRed = IM_COL32(255, 0, 0, 180);
    const ImU32 colorDarkGreen = IM_COL32(0, 40, 0, 120);
    const ImU32 colorDarkOrange = IM_COL32(50, 30, 0, 120);
    const ImU32 colorDarkRed = IM_COL32(50, 0, 0, 120);
    const ImU32 colorBackground = IM_COL32(10, 10, 10, 200);

    for (int ch = 0; ch < numChannels; ++ch) {
        float x = cursorPos.x + ch * (channelWidth + spacing);
        float y = cursorPos.y;

        // Draw background
        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + channelWidth, y + height),
                                colorBackground);

        // --- Draw VU Meter (Background) ---
        float level = std::min(vuLevels[ch], 1.5f);
        float normalizedLevel = std::min(level, 1.0f);

        int numSegments = std::max(8, static_cast<int>(height / 6.0f));
        float segmentGap = 1.0f;
        float segmentHeight = (height - (numSegments - 1) * segmentGap) / numSegments;
        int litSegments = static_cast<int>(normalizedLevel * numSegments + 0.5f);

        for (int seg = 0; seg < numSegments; ++seg) {
            float segY = y + height - (seg + 1) * (segmentHeight + segmentGap);
            float segmentProgress = static_cast<float>(seg) / (numSegments - 1);

            ImU32 litColor = (segmentProgress < 0.6f)   ? colorGreen
                             : (segmentProgress < 0.8f) ? colorOrange
                                                        : colorRed;
            ImU32 dimColor = (segmentProgress < 0.6f)   ? colorDarkGreen
                             : (segmentProgress < 0.8f) ? colorDarkOrange
                                                        : colorDarkRed;

            ImU32 color = (seg < litSegments) ? litColor : dimColor;

            // Draw segment
            drawList->AddRectFilled(ImVec2(x, segY), ImVec2(x + channelWidth, segY + segmentHeight),
                                    color);
        }

        // Peak indicator
        if (level > 1.0f) {
            drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + channelWidth, y + segmentHeight),
                                    IM_COL32(255, 50, 50, 255));
        }

        // --- Draw Waveform (Foreground) ---
        // Since we don't have per-channel waveform, we simulate it using master waveform * channel
        // volume
        if (!m_waveformBuffer.empty()) {
            float amp = vuLevels[ch]; // Modulate by volume
            // Scale amplitude visually
            amp = std::sqrt(amp); // Make quiet sounds more visible
            if (amp > 1.0f)
                amp = 1.0f;

            // Center Y
            float centerY = y + height * 0.5f;
            float halfHeight = height * 0.5f;

            for (size_t i = 0; i < m_waveformBuffer.size() - 1; ++i) {
                float sample1 = m_waveformBuffer[i] * amp;
                float sample2 = m_waveformBuffer[i + 1] * amp;

                float x1 =
                    x + (static_cast<float>(i) / (m_waveformBuffer.size() - 1)) * channelWidth;
                float x2 =
                    x + (static_cast<float>(i + 1) / (m_waveformBuffer.size() - 1)) * channelWidth;

                // Invert Y because screen Y grows down
                float y1 = centerY - sample1 * halfHeight;
                float y2 = centerY - sample2 * halfHeight;

                drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 255, 255, 200));
            }
        }
    }

    // Advance cursor
    ImGui::Dummy(ImVec2(availSize.x, height));
}

void AudioPlayer::DrawSamplesTab()
{
    if (m_sampleNames.empty()) {
        ImGui::TextDisabled("No samples in this module");
        return;
    }

    ImVec2 availSize = ImGui::GetContentRegionAvail();

    if (ImGui::BeginChild("##SamplesList", availSize, true)) {
        for (size_t i = 0; i < m_sampleNames.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));

            // Sample number (1-based for display, matching tracker convention)
            ImGui::Text("%02d:", static_cast<int>(i + 1));
            ImGui::SameLine();

            // Sample name (or placeholder if empty)
            if (m_sampleNames[i].empty()) {
                ImGui::TextDisabled("(empty)");
            } else {
                ImGui::TextUnformatted(m_sampleNames[i].c_str());
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void AudioPlayer::DrawPatternsTab()
{
    if (!m_module || m_numChannels == 0) {
        ImGui::TextDisabled("No pattern data available");
        return;
    }

    // Get current playback position
    core::PlaybackPosition pos = m_module->GetPlaybackPosition();

    // Header section with position info
    ImGui::Text("Order: %d/%d  Pattern: %d  Row: %d/%d", pos.currentOrder, pos.numOrders - 1,
                pos.currentPattern, pos.currentRow, pos.numRows - 1);

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
    ImGui::Checkbox("Follow", &m_followPlayback);

    // Draw order list
    DrawOrderList(pos.currentOrder, pos.numOrders);

    ImGui::Separator();

    // Pattern display area
    ImVec2 availSize = ImGui::GetContentRegionAvail();

    // Colors for the pattern display (retro tracker style)
    // Colors for the pattern display (retro tracker style)
    const ImU32 rowHighlight = IM_COL32(0, 60, 0, 255);
    const ImU32 rowAltBg = IM_COL32(20, 20, 32, 255);

    // Note colors for syntax highlighting
    const ImVec4 noteColor(0.4f, 1.0f, 0.4f, 1.0f);         // Green for notes
    const ImVec4 instrColor(1.0f, 1.0f, 0.4f, 1.0f);        // Yellow for instrument
    const ImVec4 effectColor(1.0f, 0.6f, 0.4f, 1.0f);       // Orange for effects
    const ImVec4 emptyColor(0.3f, 0.3f, 0.4f, 1.0f);        // Dim for empty/dots
    const ImVec4 rowNumColor(0.5f, 0.5f, 0.6f, 1.0f);       // Gray for row numbers
    const ImVec4 activeRowNumColor(1.0f, 1.0f, 1.0f, 1.0f); // White for active row

    // Setup table with 1 column for row numbers + 1 column per channel
    // BordersInnerV adds the vertical lines. ScrollX/Y enables scrolling with frozen headers.
    int columns = m_numChannels + 1;
    ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg;

    if (ImGui::BeginTable("##PatternTable", columns, flags, availSize)) {
        // Setup columns
        // Row number column
        float rowNumWidth = ImGui::CalcTextSize("000").x + 20.0f;
        ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed, rowNumWidth);

        // Channel columns
        // Add extra padding to the right of the vertical line as requested
        // Cell content is ~10 chars ("C-5 01 v64").
        // We ensure the column is wide enough and the content is left-aligned,
        // essentially creating space on the right before the next vertical line.
        float channelContentWidth = ImGui::CalcTextSize("C-5 01 v64 ...").x;
        float channelColWidth = channelContentWidth + 30.0f; // Add 30px padding on the right

        for (int ch = 0; ch < m_numChannels; ++ch) {
            std::string headerName = (ch < static_cast<int>(m_channelNames.size()))
                                         ? m_channelNames[ch]
                                         : "Ch " + std::to_string(ch + 1);
            if (headerName.length() > 12)
                headerName = headerName.substr(0, 11) + ".";

            // Use ImGuiTableColumnFlags_WidthFixed to enforce our calculated spacing
            ImGui::TableSetupColumn(headerName.c_str(), ImGuiTableColumnFlags_WidthFixed,
                                    channelColWidth);
        }

        // Freeze the header row
        ImGui::TableSetupScrollFreeze(1, 1); // 1 column (Row num), 1 row (Headers)

        // Draw Headers
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        // Row Number Header
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Row");

        // Channel Headers with Mute Toggle
        for (int ch = 0; ch < m_numChannels; ++ch) {
            ImGui::TableSetColumnIndex(ch + 1);

            bool isMuted = m_module->IsChannelMuted(ch);
            std::string label = ImGui::TableGetColumnName(ch + 1);

            // Visual mute indication
            if (isMuted) {
                label += " (M)";
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 50, 50, 255));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, noteColor);
            }

            // Clickable header to toggle mute
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None)) {
                m_module->SetChannelMute(ch, !isMuted);
            }
            ImGui::PopStyleColor();
        }

        // Draw Pattern Rows
        // Draw Pattern Rows
        int numRows = pos.numRows;
        ImGuiListClipper clipper;
        clipper.Begin(numRows);

        // Auto-scroll logic
        if (m_followPlayback && m_engine && m_engine->IsPlaying() && !m_engine->IsPaused()) {
            if (pos.currentRow != m_lastScrolledRow) {
                // Approximate scroll position
                float itemHeight = ImGui::GetTextLineHeightWithSpacing();
                float scrollY = (pos.currentRow - 4) * itemHeight;
                ImGui::SetScrollY(std::max(0.0f, scrollY));
                m_lastScrolledRow = pos.currentRow;
            }
        }

        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow();

                bool isCurrentRow = (row == pos.currentRow);

                // Highlight current row background
                if (isCurrentRow) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowHighlight);
                } else if (row % 4 == 0) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowAltBg);
                }

                // Row Number
                ImGui::TableSetColumnIndex(0);
                if (isCurrentRow) {
                    ImGui::TextColored(activeRowNumColor, "%02X", row);
                } else {
                    ImGui::TextColored(rowNumColor, "%02X", row);
                }

                // Channel Data
                for (int ch = 0; ch < m_numChannels; ++ch) {
                    ImGui::TableSetColumnIndex(ch + 1);

                    bool isMuted = m_module->IsChannelMuted(ch);

                    std::string cellData =
                        m_module->GetPatternRowChannel(pos.currentPattern, row, ch);

                    // If muted, dim the whole cell
                    if (isMuted) {
                        ImGui::TextDisabled("%s", cellData.c_str());
                    } else {
                        // Regular colored parsing
                        if (cellData.empty() ||
                            cellData.find_first_not_of(". ") == std::string::npos) {
                            ImGui::TextColored(emptyColor, "... .. ... ...");
                        } else {
                            // Copied parsing logic
                            size_t len = cellData.length();

                            // Note
                            if (len >= 3) {
                                std::string note = cellData.substr(0, 3);
                                bool hasNote = (note.find_first_not_of(". ") != std::string::npos);
                                ImGui::TextColored(hasNote ? noteColor : emptyColor, "%s",
                                                   note.c_str());
                                ImGui::SameLine(0, 0);
                            }

                            ImGui::TextColored(emptyColor, " ");
                            ImGui::SameLine(0, 0);

                            // Instrument
                            if (len >= 6) {
                                std::string instr = cellData.substr(4, 2);
                                bool hasInstr =
                                    (instr.find_first_not_of(". ") != std::string::npos);
                                ImGui::TextColored(hasInstr ? instrColor : emptyColor, "%s",
                                                   instr.c_str());
                                ImGui::SameLine(0, 0);
                            }

                            ImGui::TextColored(emptyColor, " ");
                            ImGui::SameLine(0, 0);

                            // Effect
                            if (len > 7) {
                                std::string rest = cellData.substr(7);
                                bool hasEffect =
                                    (rest.find_first_not_of(". ") != std::string::npos);
                                ImGui::TextColored(hasEffect ? effectColor : emptyColor, "%s",
                                                   rest.c_str());
                            }
                        }
                    }
                }
            }
        }
        ImGui::EndTable();
    }
}

void AudioPlayer::DrawOrderList(int currentOrder, int numOrders)
{
    if (numOrders <= 0 || !m_module) {
        return;
    }

    ImGui::Text("Order:");
    ImGui::SameLine();

    // Calculate how many orders we can fit
    float availWidth = ImGui::GetContentRegionAvail().x;
    float itemWidth = ImGui::CalcTextSize("000").x + 8;
    int visibleOrders = static_cast<int>(availWidth / itemWidth);
    visibleOrders = std::max(1, visibleOrders);

    // Center around current order
    int startOrder = currentOrder - visibleOrders / 2;
    startOrder = std::max(0, std::min(startOrder, numOrders - visibleOrders));
    int endOrder = std::min(startOrder + visibleOrders, numOrders);

    for (int ord = startOrder; ord < endOrder; ++ord) {
        if (ord > startOrder) {
            ImGui::SameLine(0, 2);
        }

        int pattern = m_module->GetOrderPattern(ord);

        // Push unique ID for this button based on order index
        ImGui::PushID(ord);

        bool clicked = false;
        if (ord == currentOrder) {
            // Highlight current order
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 100, 0, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 120, 0, 255));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
            if (ImGui::SmallButton(std::to_string(pattern).c_str())) {
                clicked = true;
            }
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 40, 50, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 60, 80, 255));
            if (ImGui::SmallButton(std::to_string(pattern).c_str())) {
                clicked = true;
            }
            ImGui::PopStyleColor(2);
        }

        if (clicked) {
            m_module->SetPositionOrderRow(ord, 0);
        }

        ImGui::PopID();
    }
}

std::string AudioPlayer::FormatTime(double seconds)
{
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    int totalSeconds = static_cast<int>(seconds);
    int minutes = totalSeconds / 60;
    int secs = totalSeconds % 60;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, secs);
    return buffer;
}

} // namespace ui
} // namespace adm
