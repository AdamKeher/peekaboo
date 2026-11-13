#include "DebuggerMemoryView.h"
#include <fmt/core.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <cstdio>
#include <cstdlib>
#include "../ColorManager.h"
#include "../FontRoles.h"
#include "../IconsFontAwesome6.h"
#include "../MenuIcons.h"
#include "../RegionColors.h"
#include "../TableLayout.h"
#include "EmulatorView.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

DebuggerMemoryView::DebuggerMemoryView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

void DebuggerMemoryView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void DebuggerMemoryView::SetMemorySearcher(core::MemorySearcher* searcher)
{
    m_memorySearcher = searcher;
}

void DebuggerMemoryView::SetOnMemoryTabActive(std::function<void()> callback)
{
    m_onMemoryTabActive = std::move(callback);
}

void DebuggerMemoryView::MarkRegionsDirty()
{
    m_memRegionsDirty = true;
    m_resetPendingCount = 10; // Keep refreshing for 10 frames to catch PuAE bank updates
}

void DebuggerMemoryView::ShowFindDialog()
{
    m_findMemDialogOpen = true;
    m_focusMemFindInput = true;
    m_open = true;
    m_shouldFocus = true;
}

void DebuggerMemoryView::FindNext() { PerformMemorySearch(false); }

void DebuggerMemoryView::FindPrevious() { PerformMemorySearch(true); }

void DebuggerMemoryView::PerformMemorySearch(bool reverse)
{
    if (!m_memorySearcher || !m_memorySearcher->HasRegions())
        return;

    std::vector<uint8_t> pattern;
    if (m_lastMemSearchType == core::DiskSearcher::SearchType::ASCII) {
        if (m_memSearchAscii.empty())
            return;
        pattern.assign(m_memSearchAscii.begin(), m_memSearchAscii.end());
    } else {
        if (m_memSearchHex.empty())
            return;
        pattern = core::DiskSearcher::ParseHex(m_memSearchHex);
        if (pattern.empty())
            return;
    }

    uint32_t startAddr = 0;
    if (m_hasMemSearchResult) {
        if (!reverse) {
            startAddr = m_memSearchResultAddr + 1;
        } else {
            startAddr = m_memSearchResultAddr > 0 ? m_memSearchResultAddr - 1 : 0xFFFFFFFFu;
        }
    } else {
        if (reverse)
            startAddr = 0xFFFFFFFFu;
    }

    uint32_t outAddr = 0;
    size_t outLength = 0;
    if (m_memorySearcher->FindNext(pattern, m_isMemCaseSensitive, reverse, startAddr, outAddr,
                                   outLength)) {
        HighlightMemoryResult(outAddr, outLength);
    }
}

void DebuggerMemoryView::HighlightMemoryResult(uint32_t amigaAddr, size_t length, bool focus)
{
    m_open = true;
    if (focus)
        m_shouldFocus = true;

    if (m_memRegionsDirty && m_emulatorView && m_emulatorView->IsRunning()) {
        auto fresh = m_emulatorView->GetFreshMemoryRegions();
        m_cachedMemRegions = std::move(fresh);
        m_memRegionsDirty = false;
    }

    for (int i = 0; i < (int)m_cachedMemRegions.size(); ++i) {
        const auto& r = m_cachedMemRegions[i];
        if (amigaAddr >= r.amigaAddr && amigaAddr < r.amigaAddr + r.size) {
            if (m_selectedRegionIdx != i)
                SelectMemoryRegionInternal(i);

            m_hasMemSearchResult = true;
            m_memSearchResultAddr = amigaAddr;
            m_memSearchResultLength = length;
            m_memScrollToOffset = (int)(amigaAddr - r.amigaAddr);
            return;
        }
    }
}

void DebuggerMemoryView::SelectMemoryRegionInternal(int regionIdx)
{
    if (regionIdx < 0 || regionIdx >= (int)m_cachedMemRegions.size())
        return;
    const auto& r = m_cachedMemRegions[regionIdx];

    m_selectedRegionIdx = regionIdx;
    m_selectedAmigaBase = r.amigaAddr;
    m_memSnapshot.assign(r.hostPtr, r.hostPtr + r.size);

    int currentBpl = m_memHexState.BytesPerLine > 0 ? m_memHexState.BytesPerLine : 16;
    m_memHexState = {};
    m_memHexState.BytesPerLine = currentBpl;
    m_memHexState.AddressChars = 7;
    m_memHexState.LowercaseBytes = true;
    m_memHexState.RenderZeroesDisabled = false;
    m_memHexState.ShowAscii = true;
    m_memHexState.UserData = this;

    // Appended below the hex editor's built-in Copy item (see ContextMenuCallback in
    // imgui_hex.h): the editor owns the popup, so lock actions have to be handed to it rather
    // than opened from out here, where the right-click has already been consumed.
    m_memHexState.ContextMenuCallback = [](ImGuiHexEditorState* st) {
        static_cast<DebuggerMemoryView*>(st->UserData)->DrawHexContextMenu();
    };

    m_memHexState.GetAddressNameCallback = [](ImGuiHexEditorState* st, int offset, char* buf,
                                              int sz) -> bool {
        auto* self = static_cast<DebuggerMemoryView*>(st->UserData);
        snprintf(buf, sz, "$%06X", self->m_selectedAmigaBase + static_cast<uint32_t>(offset));
        return true;
    };

    m_memHexState.SingleHighlightCallback = [](ImGuiHexEditorState* st, int offset, ImColor* bg,
                                               ImColor* text,
                                               ImColor*) -> ImGuiHexEditorHighlightFlags {
        auto* self = static_cast<DebuggerMemoryView*>(st->UserData);
        *text = ImColor(IM_COL32(255, 255, 255, 255));
        if (st->Bytes && offset < (int)self->m_memSnapshot.size()) {
            auto cur = static_cast<const uint8_t*>(st->Bytes)[offset];
            if (cur != self->m_memSnapshot[offset]) {
                *bg = ImColor(255, 100, 50, 80);
                *text = ImColor(255, 100, 50, 255);
                return ImGuiHexEditorHighlightFlags_Apply | ImGuiHexEditorHighlightFlags_Ascii;
            }
        }
        // A byte that just changed keeps the change colour - that is the more urgent signal, and
        // on a Block lock it also means the lock is not holding. Otherwise tint locked bytes.
        if (self->m_showLocks && self->m_lockUi.Valid()) {
            const uint32_t addr = self->m_selectedAmigaBase + static_cast<uint32_t>(offset);
            if (const core::LockEntry* e = self->m_lockUi.At(addr)) {
                const ImVec4 c = LockModeColor(e->mode);
                *text = ImColor(c.x, c.y, c.z, 1.0f);
                return ImGuiHexEditorHighlightFlags_Apply | ImGuiHexEditorHighlightFlags_Ascii;
            }
        }
        return ImGuiHexEditorHighlightFlags_None;
    };

    m_memHexStateValid = true;
}

void DebuggerMemoryView::Draw()
{
    if (!m_open)
        return;

    if (m_shouldFocus) {
        ImGui::SetNextWindowFocus();
        m_shouldFocus = false;
    }

    ImGui::SetNextWindowSize(ImVec2(720, 576), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (ImGui::Begin(ICON_FA_MEMORY " Memory###Memory##debugger", &m_open, flags)) {
        bool nowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        bool appearing = ImGui::IsWindowAppearing();
        bool justFocused = nowFocused && !m_wasMemoryFocused;

        if (m_turboSuppressed) {
            DrawTurboSuppressedPlaceholder();
        } else {
            DrawContent();
        }

        if (m_findMemDialogOpen)
            ImGui::OpenPopup("##FindMemoryDebugger");

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("##FindMemoryDebugger", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            m_findMemDialogOpen = false;
            ImGui::Text("Search Emulator memory for:");

            if (m_focusMemFindInput) {
                ImGui::SetKeyboardFocusHere(0);
                m_focusMemFindInput = false;
            }

            ImGuiInputTextFlags inputFlags =
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
            bool triggerNext = false;

            int st = (int)m_lastMemSearchType;
            ImGui::RadioButton("ASCII##mtype", &st, (int)core::DiskSearcher::SearchType::ASCII);
            ImGui::SameLine();
            ImGui::RadioButton("Hex##mtype", &st, (int)core::DiskSearcher::SearchType::Hex);
            m_lastMemSearchType = (core::DiskSearcher::SearchType)st;

            if (m_lastMemSearchType == core::DiskSearcher::SearchType::ASCII) {
                if (ImGui::InputText("##msearchtext", &m_memSearchAscii, inputFlags))
                    triggerNext = true;
            } else {
                if (ImGui::InputText("##msearchhex", &m_memSearchHex, inputFlags))
                    triggerNext = true;
            }

            ImGui::BeginDisabled(m_lastMemSearchType == core::DiskSearcher::SearchType::Hex);
            ImGui::Checkbox("Case Sensitive##mem", &m_isMemCaseSensitive);
            ImGui::EndDisabled();

            if (ImGui::Button("Find Next") || triggerNext) {
                PerformMemorySearch(false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Find Previous")) {
                PerformMemorySearch(true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

        if ((appearing || justFocused) && m_onMemoryTabActive)
            m_onMemoryTabActive();

        m_wasMemoryFocused = nowFocused;
    }
    ImGui::End();
}

void DebuggerMemoryView::DrawContent()
{
    bool running = m_emulatorView && m_emulatorView->IsRunning();

    if ((m_memRegionsDirty || m_resetPendingCount > 0) && running) {
        auto fresh = m_emulatorView->GetFreshMemoryRegions();

        if (m_resetPendingCount > 0)
            m_resetPendingCount--;

        if (m_selectedRegionIdx >= (int)fresh.size() ||
            (!fresh.empty() && m_selectedRegionIdx >= 0 &&
             (fresh[m_selectedRegionIdx].amigaAddr != m_selectedAmigaBase ||
              fresh[m_selectedRegionIdx].size != (uint32_t)m_memSnapshot.size()))) {
            m_selectedRegionIdx = -1;
            m_memHexStateValid = false;
            m_memSnapshot.clear();
        }

        m_cachedMemRegions = std::move(fresh);
        m_memRegionsDirty = false;
    }

    if (!running && !m_cachedMemRegions.empty()) {
        m_cachedMemRegions.clear();
        m_selectedRegionIdx = -1;
        m_memHexStateValid = false;
        m_memSnapshot.clear();
        m_memRegionsDirty = true;
        m_hasMemSearchResult = false;
    }

    const auto& regions = m_cachedMemRegions;

    const float toolbarHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

    if (ImGui::BeginTabBar("MemoryTabs")) {
        ImGuiTabItemFlags inspectorFlags = m_forceInspectorTab ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Inspector", nullptr, inspectorFlags)) {
            m_forceInspectorTab = false;
            const float contentH = std::max(ImGui::GetContentRegionAvail().y - toolbarHeight, 0.0f);
            ImGui::BeginChild("##memtabcontent", ImVec2(0.0f, contentH), false,
                              ImGuiWindowFlags_NoScrollbar);

            if (regions.empty()) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                const char* msg = running ? "Loading..." : "No emulation running";
                ImVec2 textSize = ImGui::CalcTextSize(msg);
                ImGui::SetCursorPos(
                    ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
                ImGui::TextDisabled("%s", msg);
            } else {
                const float rowH = ImGui::GetTextLineHeightWithSpacing();
                float tableH = rowH * (std::min((int)regions.size(), 8) + 1) +
                               ImGui::GetStyle().CellPadding.y * 4.0f;

                ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_SizingFixedFit |
                                             ImGuiTableFlags_ScrollY | kAdjustableColumns;

                if (ImGui::BeginTable("##memregions", 6, tableFlags, ImVec2(0.0f, tableH))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn(
                        "##clr", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,
                        14.0f);
                    ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                    ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                    ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed, 44.0f);
                    ImGui::TableHeadersRow();

                    for (int i = 0; i < (int)regions.size(); i++) {
                        const auto& r = regions[i];
                        uint32_t endAddr = r.amigaAddr + r.size - 1;
                        bool isSelected = (m_selectedRegionIdx == i);

                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        {
                            ImU32 c = GetMemRegionColor(i);
                            float sz = ImGui::GetTextLineHeight() * 0.72f;
                            ImVec2 p = ImGui::GetCursorScreenPos();
                            p.y += (ImGui::GetTextLineHeightWithSpacing() - sz) * 0.5f;
                            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz),
                                                                      c, 2.0f);
                            ImGui::Dummy(ImVec2(14.0f, 0.0f));
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(i);
                        if (ImGui::Selectable(r.name.c_str(), isSelected,
                                              ImGuiSelectableFlags_SpanAllColumns)) {
                            if (!isSelected) {
                                SelectMemoryRegionInternal(i);
                                m_hasMemSearchResult = false;
                            }
                        }
                        ImGui::PopID();

                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("$%06X", r.amigaAddr);

                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("$%06X", endAddr);

                        ImGui::TableSetColumnIndex(4);
                        if (r.size >= 1024u * 1024u)
                            ImGui::Text("%u MB", r.size / (1024u * 1024u));
                        else
                            ImGui::Text("%u KB", r.size / 1024u);

                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextUnformatted(r.writable ? "R/W" : "R/O");
                    }

                    ImGui::EndTable();
                }

                ImGui::Separator();

                if (m_memHexStateValid && m_selectedRegionIdx >= 0 &&
                    m_selectedRegionIdx < (int)regions.size()) {
                    const auto& r = regions[m_selectedRegionIdx];
                    m_memHexState.Bytes = r.hostPtr;
                    m_memHexState.MaxBytes = static_cast<int>(r.size);
                    m_memHexState.ReadOnly = !r.writable;
                    m_selectedAmigaBase = r.amigaAddr;

                    m_memHexState.HighlightRanges.clear();
                    if (m_hasMemSearchResult && m_memSearchResultAddr >= r.amigaAddr &&
                        m_memSearchResultAddr < r.amigaAddr + r.size) {
                        int fromOff = (int)(m_memSearchResultAddr - r.amigaAddr);
                        int toOff =
                            fromOff +
                            (m_memSearchResultLength > 0 ? (int)m_memSearchResultLength - 1 : 0);
                        ImGuiHexEditorHighlightRange hr;
                        hr.From = fromOff;
                        hr.To = toOff;
                        hr.Color = IM_COL32(255, 255, 0, 80);
                        hr.BorderColor = IM_COL32(255, 255, 0, 255);
                        hr.Flags = ImGuiHexEditorHighlightFlags_Border |
                                   ImGuiHexEditorHighlightFlags_FullSized |
                                   ImGuiHexEditorHighlightFlags_OverrideBorderColor |
                                   ImGuiHexEditorHighlightFlags_Ascii;
                        m_memHexState.HighlightRanges.push_back(hr);
                    }

                    // Locked ranges, in the same two colours the register windows and the
                    // Trainer table use: red blocks writes, amber forces a value. A disabled
                    // entry is drawn faintly rather than hidden, so "it is set up but off" is
                    // distinguishable from "there is no lock here".
                    if (m_showLocks && m_lockUi.Valid()) {
                        for (const auto& e : m_lockUi.locks->All()) {
                            if (!e.resolved)
                                continue;
                            const uint32_t from = *e.resolved;
                            const uint32_t span = e.Span();
                            const uint32_t regionEnd = r.amigaAddr + static_cast<uint32_t>(r.size);
                            if (span == 0 || from + span <= r.amigaAddr || from >= regionEnd)
                                continue;
                            const uint32_t clampedFrom = from < r.amigaAddr ? r.amigaAddr : from;
                            const uint32_t clampedEnd =
                                from + span < regionEnd ? from + span : regionEnd;
                            const ImVec4 c = LockModeColor(e.mode);
                            ImGuiHexEditorHighlightRange lr;
                            lr.From = static_cast<int>(clampedFrom - r.amigaAddr);
                            lr.To = static_cast<int>(clampedEnd - 1 - r.amigaAddr);
                            lr.Color = ImGui::ColorConvertFloat4ToU32(
                                ImVec4(c.x, c.y, c.z, e.enabled ? 0.25f : 0.10f));
                            lr.BorderColor = ImGui::ColorConvertFloat4ToU32(c);
                            lr.Flags = ImGuiHexEditorHighlightFlags_Border |
                                       ImGuiHexEditorHighlightFlags_FullSized |
                                       ImGuiHexEditorHighlightFlags_OverrideBorderColor |
                                       ImGuiHexEditorHighlightFlags_Ascii;
                            m_memHexState.HighlightRanges.push_back(lr);
                        }
                    }

                    {
                        ImVec4 barCol =
                            ImGui::ColorConvertU32ToFloat4(GetMemRegionColor(m_selectedRegionIdx));
                        ImVec2 hexAvail = ImGui::GetContentRegionAvail();
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, barCol);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                        ImGui::BeginChild("##memhexbar", ImVec2(4.0f, hexAvail.y), false,
                                          ImGuiWindowFlags_NoScrollbar);
                        ImGui::EndChild();
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor();
                        ImGui::SameLine(0, 4);
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ColorManager::Instance().GetColor("MintGreen"));
                    PushFontRole(FontRole::Memory);
                    if (ImGui::BeginHexEditor("##memhex", &m_memHexState)) {
                        if (m_memScrollToOffset >= 0) {
                            int bpl =
                                m_memHexState.BytesPerLine > 0 ? m_memHexState.BytesPerLine : 16;
                            float rowH2 = ImGui::GetTextLineHeightWithSpacing();
                            ImGui::SetScrollY((float)(m_memScrollToOffset / bpl) * rowH2);
                            m_memScrollToOffset = -1;
                        }
                    }
                    ImGui::EndHexEditor();
                    PopFontRole();
                    ImGui::PopStyleColor();
                } else {
                    ImVec2 avail = ImGui::GetContentRegionAvail();
                    const char* msg = "Select a memory region above to view it";
                    ImVec2 textSize = ImGui::CalcTextSize(msg);
                    ImGui::SetCursorPos(
                        ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
                    ImGui::TextDisabled("%s", msg);
                }
            }

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Allocations")) {
            const float contentH = std::max(ImGui::GetContentRegionAvail().y - toolbarHeight, 0.0f);
            ImGui::BeginChild("##memstatcontent", ImVec2(0.0f, contentH), false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            DrawStatsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    DrawToolbar();
    DrawSetValueDialog();
}

void DebuggerMemoryView::Clear()
{
    if (!m_cachedMemRegions.empty()) {
        m_cachedMemRegions.clear();
        m_memSnapshot.clear();
        m_selectedRegionIdx = -1;
        m_memRegionsDirty = true;
        m_memHexStateValid = false;
        m_hasMemSearchResult = false;
    }
    m_parsedMemHeaders.clear();
    m_parsedExecBase = 0;
    m_parsedMemList = 0;
    m_memAllocationsDirty = true;
}

void DebuggerMemoryView::DrawToolbar()
{
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() -
                         ImGui::GetStyle().WindowPadding.y);
    ImGui::Separator();

    ImGui::BeginDisabled(!m_memHexStateValid);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("Bytes per line",
                          fmt::format("{} bytes", m_memHexState.BytesPerLine).c_str())) {
        int options[] = {16, 24, 32};
        for (int opt : options) {
            bool isSelected = (m_memHexState.BytesPerLine == opt);
            if (ImGui::Selectable(fmt::format("{} bytes", opt).c_str(), isSelected)) {
                m_memHexState.BytesPerLine = opt;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Find")) {
        ShowFindDialog();
    }

    if (m_lockUi.Valid()) {
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_LOCK " Locks", &m_showLocks);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Outline locked ranges: red blocks writes, amber forces a value.\n"
                              "Right-click a selection to add or remove one.");
        }
        if (!m_lockStatus.empty()) {
            ImGui::SameLine();
            if (m_lockStatusIsError)
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_lockStatus.c_str());
            else
                ImGui::TextDisabled("%s", m_lockStatus.c_str());
        }
    }
}

bool DebuggerMemoryView::SelectedRange(uint32_t& addr, uint32_t& len) const
{
    if (!m_memHexStateValid || m_memHexState.SelectStartByte < 0)
        return false;
    const int from = m_memHexState.SelectStartByte;
    const int to = m_memHexState.SelectEndByte < from ? from : m_memHexState.SelectEndByte;
    addr = m_selectedAmigaBase + static_cast<uint32_t>(from);
    len = static_cast<uint32_t>(to - from + 1);
    return true;
}

void DebuggerMemoryView::DrawHexContextMenu()
{
    if (!m_lockUi.Valid())
        return;

    uint32_t addr = 0;
    uint32_t len = 0;
    const bool haveSel = SelectedRange(addr, len);
    const bool haveLock = haveSel && m_lockUi.At(addr) != nullptr;

    if (MenuItemIcon(ICON_FA_LOCK, "Lock range (block writes)", nullptr, false, haveSel)) {
        std::string err;
        if (AddBlockLock(m_lockUi, addr, len, fmt::format("block ${:08X}", addr), err)) {
            m_lockStatus = fmt::format("Blocked {} byte(s) at ${:08X}.", len, addr);
            m_lockStatusIsError = false;
        } else {
            m_lockStatus = err;
            m_lockStatusIsError = true;
        }
    }

    if (MenuItemIcon(ICON_FA_PEN, "Set value...", nullptr, false, haveSel)) {
        m_lockDialogAddr = addr;
        // A 1/2/4-byte selection picks its own width; anything else falls back to a byte.
        m_lockDialogSize = (len == 2 || len == 4) ? static_cast<int>(len) : 1;
        m_lockDialogValue = 0;
        if (m_memHexState.Bytes) {
            const auto* bytes = static_cast<const uint8_t*>(m_memHexState.Bytes);
            const int off = m_memHexState.SelectStartByte;
            for (int i = 0; i < m_lockDialogSize && off + i < m_memHexState.MaxBytes; ++i)
                m_lockDialogValue = (m_lockDialogValue << 8) | bytes[off + i];
        }
        m_openSetValuePopup = true;
    }

    if (MenuItemIcon(ICON_FA_TRASH, "Remove lock", nullptr, false, haveLock)) {
        const size_t n = RemoveLocksAt(m_lockUi, addr);
        m_lockStatus = fmt::format("Removed {} lock(s) at ${:08X}.", n, addr);
        m_lockStatusIsError = false;
    }
}

void DebuggerMemoryView::DrawSetValueDialog()
{
    // Opened from inside the hex editor's own popup, which has to close first - so the request
    // is staged there and the modal is opened here, on the next frame.
    if (m_openSetValuePopup) {
        ImGui::OpenPopup("Set value##memlock");
        m_openSetValuePopup = false;
    }
    if (!ImGui::BeginPopupModal("Set value##memlock", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Force a value at $%08X", m_lockDialogAddr);
    ImGui::Separator();

    const char* sizes[] = {"8-bit", "16-bit", "32-bit"};
    int sizeIdx = m_lockDialogSize == 4 ? 2 : (m_lockDialogSize == 2 ? 1 : 0);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::Combo("Size", &sizeIdx, sizes, IM_ARRAYSIZE(sizes)))
        m_lockDialogSize = sizeIdx == 2 ? 4 : (sizeIdx == 1 ? 2 : 1);

    const int digits = m_lockDialogSize * 2;
    char buf[16];
    snprintf(buf, sizeof(buf), "%0*X", digits, m_lockDialogValue);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputText("Value (hex)", buf, sizeof(buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                             ImGuiInputTextFlags_AutoSelectAll)) {
        m_lockDialogValue = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
    }

    // Truncating silently would lock a value the user never asked for, so say so and refuse.
    const uint32_t maxValue =
        m_lockDialogSize == 1 ? 0xFFu : (m_lockDialogSize == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    const bool fits = m_lockDialogValue <= maxValue;
    if (!fits) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "$%X does not fit in %d bits",
                           m_lockDialogValue, m_lockDialogSize * 8);
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!fits);
    if (ImGui::Button("Lock", ImVec2(90, 0))) {
        std::string err;
        if (AddSetLock(
                m_lockUi, m_lockDialogAddr, static_cast<uint8_t>(m_lockDialogSize),
                m_lockDialogValue,
                fmt::format("set ${:08X} = ${:0{}X}", m_lockDialogAddr, m_lockDialogValue, digits),
                err)) {
            m_lockStatus = fmt::format("Forcing ${:0{}X} at ${:08X}.", m_lockDialogValue, digits,
                                       m_lockDialogAddr);
            m_lockStatusIsError = false;
        } else {
            m_lockStatus = err;
            m_lockStatusIsError = true;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

bool DebuggerMemoryView::Read32(uint32_t amigaAddr, uint32_t& out_val) const
{
    for (const auto& r : m_cachedMemRegions) {
        if (amigaAddr >= r.amigaAddr && amigaAddr + 4 <= r.amigaAddr + r.size) {
            if (r.hostPtr) {
                uint32_t offset = amigaAddr - r.amigaAddr;
                out_val = (static_cast<uint32_t>(r.hostPtr[offset]) << 24) |
                          (static_cast<uint32_t>(r.hostPtr[offset + 1]) << 16) |
                          (static_cast<uint32_t>(r.hostPtr[offset + 2]) << 8) |
                          static_cast<uint32_t>(r.hostPtr[offset + 3]);
                return true;
            }
        }
    }
    return false;
}

bool DebuggerMemoryView::Read16(uint32_t amigaAddr, uint16_t& out_val) const
{
    for (const auto& r : m_cachedMemRegions) {
        if (amigaAddr >= r.amigaAddr && amigaAddr + 2 <= r.amigaAddr + r.size) {
            if (r.hostPtr) {
                uint32_t offset = amigaAddr - r.amigaAddr;
                out_val = static_cast<uint16_t>((static_cast<uint16_t>(r.hostPtr[offset]) << 8) |
                                                static_cast<uint16_t>(r.hostPtr[offset + 1]));
                return true;
            }
        }
    }
    return false;
}

void DebuggerMemoryView::RefreshAllocations()
{
    m_parsedMemHeaders.clear();
    m_parsedExecBase = 0;
    m_parsedMemList = 0;

    bool running = m_emulatorView && m_emulatorView->IsRunning();
    if (!running || m_cachedMemRegions.empty())
        return;

    uint32_t execBase = 0;
    if (!Read32(0x00000004, execBase) || execBase == 0)
        return;

    m_parsedExecBase = execBase;

    uint32_t memList = execBase + 0x0142; // ExecBase offset for MemList structure
    m_parsedMemList = memList;

    uint32_t currentHeader = 0;
    if (!Read32(memList, currentHeader))
        return;

    constexpr int kMaxHeaders = 64;
    int headerCount = 0;
    while (currentHeader != 0 && headerCount++ < kMaxHeaders) {
        ParsedMemHeader header;
        header.headerAddress = currentHeader;

        uint16_t attributes = 0;
        if (!Read16(currentHeader + 0x0E, attributes))
            break;
        header.attributes = attributes;

        uint32_t lower = 0, upper = 0, firstChunk = 0;
        if (!Read32(currentHeader + 0x14, lower))
            break;
        if (!Read32(currentHeader + 0x18, upper))
            break;
        if (!Read32(currentHeader + 0x10, firstChunk))
            break;

        header.lower = lower;
        header.upper = upper;

        uint32_t currentChunk = firstChunk;
        uint32_t totalFree = 0;
        std::vector<ParsedMemChunk> freeChunks;

        constexpr int kMaxChunks = 4096;
        int chunkCount = 0;
        while (currentChunk != 0 && chunkCount++ < kMaxChunks) {
            uint32_t nextChunk = 0;
            uint32_t bytes = 0;
            if (!Read32(currentChunk, nextChunk))
                break;
            if (!Read32(currentChunk + 0x04, bytes))
                break;

            if (bytes > 0) {
                ParsedMemChunk chunk;
                chunk.address = currentChunk;
                chunk.size = bytes;
                chunk.isFree = true;
                chunk.attributes = attributes;
                freeChunks.push_back(chunk);
                totalFree += bytes;
            }

            if (nextChunk == currentChunk)
                break;
            currentChunk = nextChunk;
        }

        header.freeSpace = totalFree;

        std::vector<ParsedMemChunk> allChunks;
        std::sort(freeChunks.begin(), freeChunks.end());

        uint32_t currentAddr = header.lower;
        for (const auto& fc : freeChunks) {
            if (fc.address > currentAddr) {
                ParsedMemChunk allocChunk;
                allocChunk.address = currentAddr;
                allocChunk.size = fc.address - currentAddr;
                allocChunk.isFree = false;
                allocChunk.attributes = attributes;
                allChunks.push_back(allocChunk);
            }
            allChunks.push_back(fc);
            currentAddr = fc.address + fc.size;
        }

        if (currentAddr < header.upper) {
            ParsedMemChunk allocChunk;
            allocChunk.address = currentAddr;
            allocChunk.size = header.upper - currentAddr;
            allocChunk.isFree = false;
            allocChunk.attributes = attributes;
            allChunks.push_back(allocChunk);
        }

        header.chunks = std::move(allChunks);
        m_parsedMemHeaders.push_back(std::move(header));

        uint32_t nextHeader = 0;
        if (!Read32(currentHeader, nextHeader))
            break;

        if (nextHeader == currentHeader || nextHeader == 0)
            break;

        uint32_t nextNext = 0;
        if (!Read32(nextHeader, nextNext))
            break;
        if (nextNext == 0)
            break; // Reached lh_Tail

        currentHeader = nextHeader;
    }
}

void DebuggerMemoryView::DrawStatsTab()
{
    auto now = std::chrono::steady_clock::now();
    if (m_memAllocationsDirty ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastMemAllocRefresh).count() >
            1000) {
        RefreshAllocations();
        m_memAllocationsDirty = false;
        m_lastMemAllocRefresh = now;
    }

    if (!m_emulatorView || !m_emulatorView->IsRunning()) {
        ImGui::TextDisabled("No emulation running");
        return;
    }

    if (m_parsedExecBase == 0) {
        ImGui::TextDisabled("ExecBase not initialized (Wait for Amiga OS to boot)");
        return;
    }

    ImGui::Text("ExecBase: 0x%06X        MemList: 0x%06X", m_parsedExecBase, m_parsedMemList);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    auto FormatSize = [](uint32_t bytes) -> std::string {
        char buf[64];
        if (bytes >= 1024 * 1024) {
            snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1024.0f / 1024.0f);
        } else if (bytes >= 1024) {
            snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0f);
        } else {
            snprintf(buf, sizeof(buf), "%u B", bytes);
        }
        return std::string(buf);
    };

    struct MemGroup
    {
        std::string name;
        uint32_t total = 0;
        uint32_t free = 0;
        uint32_t used = 0;
        std::vector<ParsedMemChunk> chunks;

        explicit MemGroup(std::string n) : name(std::move(n)) {}
    };

    MemGroup chipGroup{"CHIP Memory"};
    MemGroup fastGroup{"FAST Memory"};
    MemGroup otherGroup{"OTHER Memory"};

    for (const auto& h : m_parsedMemHeaders) {
        MemGroup* group = &otherGroup;
        if (h.attributes & kMemF_Chip) {
            group = &chipGroup;
        } else if (h.attributes & kMemF_Fast) {
            group = &fastGroup;
        } else {
            if (h.lower < 0x200000)
                group = &chipGroup;
            else
                group = &fastGroup;
        }

        uint32_t total = h.upper - h.lower;
        group->total += total;
        group->free += h.freeSpace;
        group->used += (total - h.freeSpace);
        for (const auto& c : h.chunks) {
            group->chunks.push_back(c);
        }
    }

    std::vector<MemGroup*> activeGroups;
    if (chipGroup.total > 0)
        activeGroups.push_back(&chipGroup);
    if (fastGroup.total > 0)
        activeGroups.push_back(&fastGroup);
    if (otherGroup.total > 0)
        activeGroups.push_back(&otherGroup);

    for (auto* group : activeGroups) {
        ImGui::SeparatorText(group->name.c_str());

        float pct = 0.0f;
        if (group->total > 0)
            pct = ((float)group->used / group->total) * 100.0f;

        ImGui::BeginGroup();
        ImGui::TextDisabled("Total:");
        ImGui::Text("%s", FormatSize(group->total).c_str());
        ImGui::EndGroup();
        ImGui::SameLine(100.0f);

        ImGui::BeginGroup();
        ImGui::TextDisabled("Free:");
        ImGui::Text("%s", FormatSize(group->free).c_str());
        ImGui::EndGroup();
        ImGui::SameLine(200.0f);

        ImGui::BeginGroup();
        ImGui::TextDisabled("Used:");
        ImGui::Text("%s (%.1f%%)", FormatSize(group->used).c_str(), pct);
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    for (auto* group : activeGroups) {
        ImGui::SeparatorText(group->name.c_str());

        std::string tableId = "##Table_" + group->name;
        if (ImGui::BeginTable(tableId.c_str(), 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | kAdjustableColumns)) {
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Segment");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();

            for (int ci = 0; ci < (int)group->chunks.size(); ci++) {
                const auto& c = group->chunks[ci];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(ci);
                char addrStr[32];
                snprintf(addrStr, sizeof(addrStr), "0x%06X", c.address);
                if (ImGui::Selectable(addrStr, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    HighlightMemoryResult(c.address, c.size);
                    m_forceInspectorTab = true;
                }
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", FormatSize(c.size).c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("-");

                ImGui::TableSetColumnIndex(3);
                if (c.isFree) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Free");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Allocated");
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }
}

} // namespace adm::ui
