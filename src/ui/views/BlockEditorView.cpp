#include "BlockEditorView.h"
#include "../../core/BlockAnalyzer.h"
#include "../ColorManager.h"
#include "../FontRoles.h"

#include <fmt/format.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include "../IconsFontAwesome6.h"

namespace adm::ui {

BlockEditorView::BlockEditorView()
{
    // Initialize callbacks for sub-views
    m_structureView.SetOnNavigate([this](uint32_t block) {
        m_pendingNavigate = true;
        m_pendingNavigateBlock = block;
    });

    m_structureView.SetOnHover([this](int from, int to) {
        SetHoverRange(from, to);
        // Notify external listener (e.g., to sync with HexViewOrig)
        if (m_onHoverRange) {
            m_onHoverRange(from, to);
        }
    });

    // Default to 16 bytes per line for standard hex view
    m_hexState.BytesPerLine = 16;
    m_hexState.AddressChars = 8;
    m_hexState.LowercaseBytes = true;
    m_hexState.UserData = this;

    // Enable editing
    m_hexState.ReadOnly = false;
    m_hexState.EnableClipboard = true;
    m_hexState.ClipboardFlags = ImGuiHexEditorClipboardFlags_None;

    m_hexState.WriteCallback = [](ImGuiHexEditorState* state, int offset, void* buf,
                                  int size) -> int {
        auto* view = static_cast<BlockEditorView*>(state->UserData);
        if (!view)
            return 0;

        // Update local data
        // Note: m_data is accessible because lambda is inside member function or friend?
        // Wait, m_data is protected in HexViewBase. HexEditorView inherits it.
        // We need to access it via 'view'. 'view' is HexEditorView*.
        // m_data is protected. Lambda is not a member.
        // But we can cast state->Bytes which points to m_data.data().
        // Or better: use view->SetData? No, partial update.
        // Actually, HexEditorView has access to m_data. But the lambda is static-like (stateless
        // capture or simple capture). It's defined inside BlockEditorView::HexEditorView(). But
        // 'view' pointer allows access to public members. m_data is protected. We can expose a
        // public method OnWrite or similar. Or just modify state->Bytes directly? state->Bytes is
        // void*.

        if (state->Bytes && offset >= 0 && offset + size <= state->MaxBytes) {
            memcpy((uint8_t*)state->Bytes + offset, buf, size);

            // Persist
            // We need to access m_writeBlockCallback. It is private.
            // We can add a public friend or method.
            // Or just make OnWrite method public/internal.
            // Let's add a public method WriteData(offset, data) to HexEditorView.
            // Or simpler: access m_data via const cast of GetData()? No.

            // HexEditorView inherits HexViewBase.
            // Let's use a helper method in HexEditorView.
            view->OnWrite(offset, buf, size);
            return size;
        }
        return 0;
    };

    m_hexState.GetAddressNameCallback = [](ImGuiHexEditorState* state, int offset, char* buf,
                                           int size) -> bool {
        auto* view = static_cast<BlockEditorView*>(state->UserData);
        if (!view)
            return false;

        uint32_t absoluteOffset = view->GetCurrentBlock() * view->GetBlockSize() + offset;
        snprintf(buf, size, "%x:%04x", absoluteOffset >> 16, absoluteOffset & 0xFFFF);
        return true;
    };
    // Callback to restore byte text color and highlight modified bytes
    m_hexState.SingleHighlightCallback = [](ImGuiHexEditorState* state, int offset, ImColor* color,
                                            ImColor* text_color,
                                            ImColor*) -> ImGuiHexEditorHighlightFlags {
        auto* view = static_cast<BlockEditorView*>(state->UserData);
        if (!view)
            return ImGuiHexEditorHighlightFlags_None;

        *text_color = ImColor(view->m_defaultTextColor);

        // Highlight bytes that differ from original
        if (!view->m_originalData.empty() && offset >= 0 &&
            offset < (int)view->m_originalData.size() && offset < (int)view->m_data.size() &&
            view->m_data[offset] != view->m_originalData[offset]) {
            *color = ImColor(255, 100, 50, 80);
            *text_color = ImColor(255, 100, 50, 255);
            return ImGuiHexEditorHighlightFlags_Apply | ImGuiHexEditorHighlightFlags_Ascii;
        }

        return ImGuiHexEditorHighlightFlags_None;
    };
}

void BlockEditorView::SetOnReadBlock(std::function<std::vector<uint8_t>(uint32_t)> callback)
{
    m_readBlockCallback = callback;
}

void BlockEditorView::SetOnAnalyzeBlock(
    std::function<core::BlockDetails(uint32_t, std::span<const uint8_t>)> callback)
{
    m_analyzeCallback = callback;
}

void BlockEditorView::SetOnBlockChanged(std::function<void(uint32_t)> callback)
{
    m_onBlockChanged = callback;
}

void BlockEditorView::SetOnRestoreFile(std::function<bool(uint32_t)> callback)
{
    m_onRestoreFile = callback;
}

void BlockEditorView::SetIsBlockModified(std::function<bool(uint32_t)> callback)
{
    m_isBlockModified = callback;
}

void BlockEditorView::SetOnRevertBlock(std::function<bool(uint32_t)> callback)
{
    m_onRevertBlock = callback;
}

void BlockEditorView::SetOnHoverRange(std::function<void(int, int)> callback)
{
    m_onHoverRange = callback;
}

void BlockEditorView::SetIsBlockCached(std::function<bool(uint32_t)> callback)
{
    m_isBlockCached = callback;
}

void BlockEditorView::SetOnReadOriginalBlock(std::function<std::vector<uint8_t>(uint32_t)> callback)
{
    m_readOriginalBlockCallback = callback;
}

void BlockEditorView::SetOnWriteBlock(
    std::function<bool(uint32_t, uint32_t, const std::vector<uint8_t>&)> callback)
{
    m_writeBlockCallback = callback;
}

void BlockEditorView::OnWrite(int offset, void* buf, int size)
{
    if (m_data.empty() || offset < 0 || offset + size > (int)m_data.size())
        return;

    memcpy(m_data.data() + offset, buf, size);

    if (m_writeBlockCallback) {
        // We pass offset, but currently AdfHandler::WriteBlock takes whole block.
        // We pass the whole m_data which is now updated.
        // The offset param in callback is for optimization if needed later, but here we pass
        // m_data.
        m_writeBlockCallback(GetCurrentBlock(), offset, m_data);
    }

    // Re-analyze to update checksum status etc.
    if (m_analyzeCallback) {
        m_lastDetails = m_analyzeCallback(GetCurrentBlock(), m_data);
    }
    UpdateBlockState();
}

void BlockEditorView::GoToBlockForce(uint32_t block)
{
    // Bypass history check and force load
    // If we're not at the end of history, truncate forward history
    if (m_historyIndex >= 0 && m_historyIndex < (int)m_blockHistory.size() - 1) {
        m_blockHistory.resize(m_historyIndex + 1);
    }
    m_blockHistory.push_back(block);
    m_historyIndex = (int)m_blockHistory.size() - 1;

    LoadBlock(block);
}

void BlockEditorView::OnNavigate(uint32_t block) { LoadBlock(block); }

void BlockEditorView::GoToBlock(uint32_t block)
{
    // Don't add duplicate consecutive entries
    if (!m_blockHistory.empty() && m_historyIndex >= 0 && m_blockHistory[m_historyIndex] == block) {
        return;
    }

    // If we're not at the end of history, truncate forward history
    if (m_historyIndex >= 0 && m_historyIndex < (int)m_blockHistory.size() - 1) {
        m_blockHistory.resize(m_historyIndex + 1);
    }

    // Add to history
    m_blockHistory.push_back(block);
    m_historyIndex = (int)m_blockHistory.size() - 1;

    // Load the block
    LoadBlock(block);
}

void BlockEditorView::LoadBlock(uint32_t block)
{
    m_currentBlock = (int)block;

    if (m_onBlockChanged) {
        m_onBlockChanged(block);
    }

    if (m_readBlockCallback) {
        m_data = m_readBlockCallback(block);
        m_hexState.Bytes = m_data.data();
        m_hexState.MaxBytes = (int)m_data.size();

        // Load original block data for diff highlighting
        if (m_readOriginalBlockCallback) {
            m_originalData = m_readOriginalBlockCallback(block);
        } else {
            m_originalData.clear();
        }

        if (m_analyzeCallback) {
            m_lastDetails = m_analyzeCallback(block, m_data);
        }

        // Update block definition and validation state immediately
        UpdateBlockState();
    }

    LoadTrack();
}

void BlockEditorView::SetIsFFS(bool isFFS) { m_isFFS = isFFS; }

void BlockEditorView::Draw()
{
    ImGuiWindowFlags commonFlags = ImGuiWindowFlags_None;
    if (m_noFocusOnAppearing) {
        commonFlags |= ImGuiWindowFlags_NoFocusOnAppearing;
    }

    // --- HEX VIEW WINDOW ---
    if (m_isBlockOpen) {
        bool isModified = m_isBlockModified && m_isBlockModified((uint32_t)m_currentBlock);
        ImGuiWindowFlags windowFlags =
            commonFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (isModified) {
            windowFlags |= ImGuiWindowFlags_UnsavedDocument;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        if (ImGui::Begin(ICON_FA_TABLE_CELLS " Block###Block", &m_isBlockOpen, windowFlags)) {
            // Notify when window gains focus (for search context switching)
            bool nowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (nowFocused && !m_wasFocused && m_onFocused)
                m_onFocused();
            m_wasFocused = nowFocused;

            // Use base class toolbar
            DrawToolbar();

            // Restore button (custom to HexEditorView)
            // Check if data is loaded for Restore button logic
            if (!m_data.empty()) {
                // Show Restore button only for deleted files
                if (m_lastDetails.isDeletedFile && m_onRestoreFile) {
                    ImGui::SameLine();
                    ImGui::Text("|");
                    ImGui::SameLine();
                    if (ImGui::Button("Restore")) {
                        if (m_onRestoreFile((uint32_t)m_currentBlock)) {
                            // Refresh the current block to update the view
                            GoToBlock((uint32_t)m_currentBlock);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Restore deleted file '%s'", m_lastDetails.name.c_str());
                    }
                }
            }

            ImGui::Separator();

            if (m_data.empty()) {
                ImGui::TextDisabled("No data loaded.");
            } else {
                ImVec2 available = ImGui::GetContentRegionAvail();

                // Apply hover highlight (from Structure Details window)
                ApplyHoverHighlight();

                // Add search result highlight if applicable
                if (m_hasSearchResult && m_currentBlock == m_searchResultBlock) {
                    ImGuiHexEditorHighlightRange resultRange;
                    resultRange.From = m_searchResultOffset;
                    resultRange.To = m_searchResultOffset +
                                     (m_searchResultLength > 0 ? m_searchResultLength - 1 : 0);
                    resultRange.Color = IM_COL32(255, 255, 0, 80);
                    resultRange.BorderColor = IM_COL32(255, 255, 0, 255);
                    resultRange.Flags = ImGuiHexEditorHighlightFlags_Border |
                                        ImGuiHexEditorHighlightFlags_FullSized |
                                        ImGuiHexEditorHighlightFlags_OverrideBorderColor |
                                        ImGuiHexEditorHighlightFlags_Ascii;
                    m_hexState.HighlightRanges.push_back(resultRange);
                }

                // UpdateHighlights/UpdateBlockState is now called in LoadBlock
                // UpdateHighlights();

                // Reserve space for status bar at bottom
                float statusBarHeight = GetBottomHeight();
                ImVec2 hexSize = ImVec2(available.x, available.y - statusBarHeight);

                // Hex data scrolls in both directions between the fixed toolbar and status bar
                DrawBlockData(hexSize);

                // Status bar with Modified indicator
                ImGui::Separator();
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::BeginCombo("Bytes per line",
                                      fmt::format("{} bytes", m_hexState.BytesPerLine).c_str())) {
                    constexpr int options[] = {16, 24, 32};
                    for (int option : options) {
                        bool selected = m_hexState.BytesPerLine == option;
                        if (ImGui::Selectable(fmt::format("{} bytes", option).c_str(), selected))
                            m_hexState.BytesPerLine = option;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                bool showTrack = m_showTrack && TrackViewAvailable();
                ImGui::BeginDisabled(!TrackViewAvailable());
                if (ImGui::Checkbox("Track", &showTrack))
                    SetShowTrack(showTrack);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(TrackViewAvailable()
                                          ? "Show every sector of this track and side"
                                          : "A floppy's view: a hardfile's track is hundreds of "
                                            "sectors");
                ImGui::SameLine();
                bool isModified = m_isBlockModified && m_isBlockModified((uint32_t)m_currentBlock);
                // Check if block is cached (unread implies not modified yet)
                bool isCached = !m_isBlockCached || m_isBlockCached((uint32_t)m_currentBlock);

                if (!isCached) {
                    ImGui::TextDisabled("Unread");
                } else if (isModified) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 50, 255));
                    ImGui::Text("Modified");
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    if (ImGui::Button("Undo")) {
                        if (m_onRevertBlock && m_onRevertBlock((uint32_t)m_currentBlock)) {
                            LoadBlock((uint32_t)m_currentBlock); // Reload to show reverted data
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Revert changes to this block from disk");
                    }
                } else {
                    ImGui::TextDisabled("Unmodified");
                }

                // Checksum Status (only for ADF blocks and bootblocks, not NDOS/raw)
                if (m_lastDetails.hasChecksum && !m_lastDetails.isChecksumValid &&
                    !m_data.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "  [Checksum Invalid]");
                    ImGui::SameLine();
                    if (ImGui::Button("Fix")) {
                        uint32_t offset = 20;
                        bool isBitmap = false;
                        for (auto b : m_bitmapBlocks)
                            if (b == (uint32_t)m_currentBlock)
                                isBitmap = true;
                        if (isBitmap)
                            offset = 0;
                        else if (m_lastDetails.isBootBlock)
                            offset = 4;

                        if (m_lastDetails.isBootBlock && m_currentBlock == 0) {
                            std::vector<uint8_t> b1 = m_readBlockCallback(1);
                            std::vector<uint8_t> combined(1024);
                            if (combined.size() == 1024 && m_data.size() >= 512 &&
                                b1.size() >= 512) {
                                memcpy(combined.data(), m_data.data(), 512);
                                memcpy(combined.data() + 512, b1.data(), 512);
                                uint32_t sum =
                                    core::BlockAnalyzer::CalculateBootBlockChecksum(combined);
                                uint8_t buf[4];
                                buf[0] = (sum >> 24) & 0xFF;
                                buf[1] = (sum >> 16) & 0xFF;
                                buf[2] = (sum >> 8) & 0xFF;
                                buf[3] = sum & 0xFF;
                                OnWrite(4, buf, 4);
                            }
                        } else if (!m_lastDetails.isBootBlock) {
                            uint32_t sum = core::BlockAnalyzer::CalculateChecksum(m_data, offset);
                            uint8_t buf[4];
                            buf[0] = (sum >> 24) & 0xFF;
                            buf[1] = (sum >> 16) & 0xFF;
                            buf[2] = (sum >> 8) & 0xFF;
                            buf[3] = sum & 0xFF;
                            OnWrite(offset, buf, 4);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Recalculate and fix checksum");
                    }
                }

                // Right-aligned bootblock info
                DrawBootBlockInfo();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    } // if (m_isBlockOpen)

    // --- BLOCK DETAILS WINDOW ---
    if (m_isBlockDetailsOpen) {
        if (ImGui::Begin(ICON_FA_TABLE_LIST " Block Details###Block Details", &m_isBlockDetailsOpen,
                         commonFlags)) {
            if (m_data.empty()) {
                ImGui::TextDisabled("No data loaded.");
            } else {
                m_structureView.Draw(m_blockDef, m_data, GetViewBaseOffset(), m_lastDetails);
            }
        }
        ImGui::End();
    }

    m_noFocusOnAppearing = false;

    // Process deferred navigation from StructureDetailsView block link clicks.
    // This must happen after Draw() returns so we don't invalidate data mid-draw.
    if (m_pendingNavigate) {
        m_pendingNavigate = false;
        GoToBlock(m_pendingNavigateBlock);
    }

    // --- FIND DIALOG ---
    if (m_findDialogOpen) {
        ImGui::OpenPopup("Find");
        m_findDialogOpen = false;
        m_focusSearchInput = true; // Flag to focus input next frame
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Find", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Search for:");

        // Handle auto-focus
        if (m_focusSearchInput) {
            ImGui::SetKeyboardFocusHere();
            m_focusSearchInput = false;
        }

        bool enterPressed = false;
        ImGuiInputTextFlags inputFlags =
            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue;

        // Input based on type
        if (m_lastSearchType == SearchType::ASCII) {
            if (ImGui::InputText("##search_ascii", &m_searchAscii, inputFlags)) {
                enterPressed = true;
            }
        } else {
            // For Hex, we could use a specific hex input or just string
            if (ImGui::InputText("##search_hex", &m_searchHex, inputFlags)) {
                enterPressed = true;
            }
        }

        // Search Type Radio
        ImGui::RadioButton("ASCII", (int*)&m_lastSearchType, (int)SearchType::ASCII);
        ImGui::SameLine();
        ImGui::RadioButton("Hex", (int*)&m_lastSearchType, (int)SearchType::Hex);

        // Case Sensitive (only for ASCII)
        ImGui::BeginDisabled(m_lastSearchType == SearchType::Hex);
        ImGui::Checkbox("Case Sensitive", &m_isCaseSensitive);
        ImGui::EndDisabled();

        ImGui::Separator();

        if (ImGui::Button("Find Next", ImVec2(120, 0))) {
            FindNext();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Find Previous", ImVec2(120, 0))) {
            FindPrevious();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }

        // Handle Enter key (if not caught by InputText, e.g. if button focused)
        // Actually InputText with EnterReturnsTrue handles it if focused.
        if (enterPressed) {
            FindNext();
            ImGui::CloseCurrentPopup();
        }

        // Handle Escape key
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void BlockEditorView::SetDiskSearcher(core::DiskSearcher* searcher) { m_diskSearcher = searcher; }

void BlockEditorView::PerformSearch(bool reverse)
{
    if (!m_diskSearcher)
        return;

    // 1. Prepare search bytes
    std::vector<uint8_t> pattern;
    if (m_lastSearchType == SearchType::ASCII) {
        if (m_searchAscii.empty())
            return;
        pattern.assign(m_searchAscii.begin(), m_searchAscii.end());
    } else {
        if (m_searchHex.empty())
            return;
        pattern = core::DiskSearcher::ParseHex(m_searchHex);
        if (pattern.empty())
            return;
    }

    uint32_t startBlock = m_currentBlock;
    size_t startOffset = reverse ? (size_t)-1 : 0;

    if (m_hasSearchResult) {
        startBlock = m_searchResultBlock;
        if (reverse) {
            if (m_searchResultOffset == 0) {
                // Determine previous block, wrap if needed
                startBlock = (startBlock == 0) ? (m_totalBlocks - 1) : (startBlock - 1);
                startOffset = (size_t)-1;
            } else {
                startOffset = m_searchResultOffset - 1;
            }
        } else {
            startOffset = m_searchResultOffset + 1;
        }
    }

    uint32_t outBlock = 0;
    size_t outOffset = 0;

    // Pass case sensitivity only for ASCII, or always if the UI supports it for Hex?
    // UI disables Case Sensitive checkbox for Hex, so m_isCaseSensitive is likely ignored or false.
    // We pass it as is.
    bool found = m_diskSearcher->FindNext(pattern, m_isCaseSensitive, reverse, startBlock,
                                          startOffset, outBlock, outOffset);

    if (found) {
        GoToBlock(outBlock);
        m_hasSearchResult = true;
        m_searchResultBlock = outBlock;
        m_searchResultOffset = outOffset;
        m_searchResultLength = pattern.size();
    }
}

void BlockEditorView::RenderBlockLink(uint32_t block)
{
    if (block == 0) {
        ImGui::TextDisabled("None");
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%u", block);

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
    ImGui::Text("%s", buf);
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::BeginTooltip();
        ImGui::Text("Click to go to block %u", block);
        ImGui::EndTooltip();
    }

    if (ImGui::IsItemClicked()) {
        GoToBlock(block);
    }
}

void BlockEditorView::RenderProtection(uint8_t access)
{
    auto bit = [&](int b, char c) {
        bool set = (access & (1 << b)) == 0;
        if (set)
            ImGui::Text("%c", c);
        else
            ImGui::TextDisabled("-");
        ImGui::SameLine(0, 0);
    };

    bit(7, 'H');
    bit(6, 'S');
    bit(5, 'P');
    bit(3, 'R');
    bit(2, 'W');
    bit(1, 'E');
    bit(0, 'D');
    ImGui::NewLine();
}

std::string BlockEditorView::DefinitionFor(uint32_t block, const core::BlockDetails& details,
                                           const std::vector<uint32_t>& bitmapBlocks) const
{
    if (block == 0 || block == 1)
        return "definitions/bootblock.json";
    if (details.secondaryType == 1)
        return "definitions/rootblock.json";
    if (details.primaryType == 2 && details.secondaryType == -3)
        return "definitions/fileheader.json";
    if (details.primaryType == 16 && details.secondaryType == -3)
        return "definitions/fileextension.json";
    if (details.primaryType == 2 && details.secondaryType == 2)
        return "definitions/userdirectory.json";
    if (details.isDataBlock)
        return m_isFFS ? "definitions/datablock_ffs.json" : "definitions/datablock_ofs.json";

    // Check if it's a bitmap block
    for (uint32_t bitmapBlock : bitmapBlocks) {
        if (block == bitmapBlock)
            return "definitions/bitmap.json";
    }

    // Heuristic for FFS data blocks: on FFS, a block that doesn't look like any header
    // structure is treated as data (the standard view shows "Unknown" for these).
    if (m_isFFS && details.primaryType != 2 && details.primaryType != 16 &&
        details.primaryType != 1) {
        return "definitions/datablock_ffs.json";
    }
    return "";
}

void BlockEditorView::UpdateBlockState()
{
    // bitmapBlocks is kept so later blocks can be recognised as bitmap blocks
    if (m_lastDetails.secondaryType == 1)
        m_bitmapBlocks = m_lastDetails.bitmapBlocks;

    std::string targetDef = DefinitionFor(m_currentBlock, m_lastDetails, m_bitmapBlocks);

    if (!targetDef.empty() && m_loadedDefinitionName != targetDef) {
        LoadDefinition(targetDef);
        m_loadedDefinitionName = targetDef;

        // Recalculate checksum validation if a field specifies checksumOffset
        if (m_blockDef.contains("fields") && !m_data.empty()) {
            for (const auto& field : m_blockDef["fields"]) {
                if (field.contains("checksumOffset")) {
                    uint32_t checksumOffset = field["checksumOffset"].get<uint32_t>();
                    m_lastDetails.isChecksumValid =
                        core::BlockAnalyzer::ValidateChecksum(m_data, checksumOffset);
                    break;
                }
            }
        }
    } else if (!targetDef.empty()) {
        ApplyHighlights();
        m_lastHighlightedBlock = m_currentBlock;

        // Recalculate checksum validation if a field specifies checksumOffset
        if (m_blockDef.contains("fields") && !m_data.empty()) {
            for (const auto& field : m_blockDef["fields"]) {
                if (field.contains("checksumOffset")) {
                    uint32_t checksumOffset = field["checksumOffset"].get<uint32_t>();
                    m_lastDetails.isChecksumValid =
                        core::BlockAnalyzer::ValidateChecksum(m_data, checksumOffset);
                    break;
                }
            }
        }
    } else if (targetDef.empty()) {
        m_blockDef = nlohmann::json();
        m_loadedDefinitionName = "";
        m_currentDefinitionFile = "";
        ClearHighlights();
        m_lastHighlightedBlock = -1;
    }
}

bool BlockEditorView::ShowsBootBlock() const
{
    if (m_currentBlock <= 1)
        return true;
    return m_showTrack && !m_trackSectors.empty() && m_trackSectors.front().block == 0;
}

void BlockEditorView::LoadTrack()
{
    m_trackSectors.clear();
    if (!m_showTrack || !TrackViewAvailable() || !m_readBlockCallback || m_data.empty())
        return;

    uint32_t sectorsPerTrack = m_sectors > 0 ? m_sectors : 11;
    uint32_t first = (m_currentBlock / sectorsPerTrack) * sectorsPerTrack;
    uint32_t count = sectorsPerTrack;
    if (m_totalBlocks > 0)
        count = std::min(count, m_totalBlocks > first ? m_totalBlocks - first : 0u);
    if (count == 0)
        return;

    // Read and analyse every sector first, so a root block on this track can name the
    // bitmap blocks that follow it.
    std::vector<uint32_t> bitmapBlocks = m_bitmapBlocks;
    std::vector<core::BlockDetails> details(count);
    m_trackSectors.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        TrackSector& sector = m_trackSectors[i];
        sector.block = first + i;
        if (sector.block == m_currentBlock)
            continue;
        sector.data = m_readBlockCallback(sector.block);
        if (m_analyzeCallback) {
            details[i] = m_analyzeCallback(sector.block, sector.data);
            sector.typeLabel = details[i].typeLabel;
            if (details[i].secondaryType == 1)
                bitmapBlocks = details[i].bitmapBlocks;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        TrackSector& sector = m_trackSectors[i];
        if (sector.block == m_currentBlock)
            continue;

        ImGuiHexEditorState& state = sector.state;
        state.Bytes = sector.data.data();
        state.MaxBytes = (int)sector.data.size();
        state.AddressChars = m_hexState.AddressChars;
        state.LowercaseBytes = m_hexState.LowercaseBytes;
        state.ReadOnly = true;
        state.EnableClipboard = true;
        state.ClipboardFlags = ImGuiHexEditorClipboardFlags_None;
        state.UserData = &sector;
        sector.textColor = &m_defaultTextColor;
        sector.blockSize = m_blockSize;

        state.GetAddressNameCallback = [](ImGuiHexEditorState* state, int offset, char* buf,
                                          int size) -> bool {
            auto* sector = static_cast<TrackSector*>(state->UserData);
            uint32_t absoluteOffset = sector->block * sector->blockSize + offset;
            snprintf(buf, size, "%x:%04x", absoluteOffset >> 16, absoluteOffset & 0xFFFF);
            return true;
        };
        state.SingleHighlightCallback = [](ImGuiHexEditorState* state, int, ImColor*,
                                           ImColor* text_color,
                                           ImColor*) -> ImGuiHexEditorHighlightFlags {
            auto* sector = static_cast<TrackSector*>(state->UserData);
            *text_color = ImColor(*sector->textColor);
            return ImGuiHexEditorHighlightFlags_None;
        };

        // Field colours without the white field outlines, as for the current block
        std::string defPath = DefinitionFor(sector.block, details[i], bitmapBlocks);
        if (!defPath.empty() && m_definitionManager) {
            nlohmann::json def = m_definitionManager->GetDefinition(defPath);
            uint32_t baseOffset =
                (defPath == "definitions/bootblock.json" && sector.block == 1) ? 0x200 : 0;
            BuildHighlightRanges(def, baseOffset, false, state.HighlightRanges, sector.fields);
        }
    }

    m_scrollToCurrentFrames = 2;
}

std::string BlockEditorView::SectorHeader(uint32_t block, const std::string& typeLabel) const
{
    uint32_t sectorsPerTrack = m_sectors > 0 ? m_sectors : 11;
    uint32_t heads = m_heads > 0 ? m_heads : 2;
    uint32_t track = block / sectorsPerTrack;
    std::string header = fmt::format("Block {}  |  Cylinder {}  Head {}  Sector {}", block,
                                     track / heads, track % heads, block % sectorsPerTrack);
    if (block <= 1)
        header += "  |  Boot block";
    else if (!typeLabel.empty())
        header += fmt::format("  |  {}", typeLabel);
    return header;
}

float BlockEditorView::HexContentWidth() const
{
    // Mirrors the layout in ImGui::BeginHexEditor: address column, byte columns with their
    // group separators, then the ASCII column.
    PushFontRole(FontRole::Memory);
    const float charWidth = ImGui::CalcTextSize("0").x;
    PopFontRole();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    const int bytesPerLine = m_hexState.BytesPerLine > 0 ? m_hexState.BytesPerLine : 16;
    int separators = m_hexState.Separators > 0 ? bytesPerLine / m_hexState.Separators : 0;
    if (separators > 0 && bytesPerLine % m_hexState.Separators == 0)
        --separators;

    float width = charWidth * (m_hexState.AddressChars + 1) + spacing * 0.5f;
    width += spacing * 0.5f + bytesPerLine * (charWidth * 2.0f + spacing) + separators * spacing;
    if (m_hexState.ShowAscii)
        width += spacing + charWidth * bytesPerLine;
    return width + spacing;
}

void BlockEditorView::DrawBlockData(const ImVec2& size)
{
    ImGui::BeginChild("##block_data", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    PushFontRole(FontRole::Memory);
    float lineHeight = ImGui::CalcTextSize("0").y + ImGui::GetStyle().ItemSpacing.y;
    PopFontRole();

    const int bytesPerLine = m_hexState.BytesPerLine > 0 ? m_hexState.BytesPerLine : 16;
    const ImGuiWindowFlags hexFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    // Each hex editor is laid out at full height and width; this child does all the scrolling
    const float width = std::max(ImGui::GetContentRegionAvail().x, HexContentWidth());

    if (!m_showTrack || m_trackSectors.empty()) {
        const int lines = ((int)m_data.size() + bytesPerLine - 1) / bytesPerLine;
        DrawHexEditor(ImVec2(width, lines * lineHeight), "##hex_editor", hexFlags);
        ImGui::EndChild();
        return;
    }

    // Every sector sits inside the same padding, so the current block's outline fits around it
    // without shifting its contents relative to the others
    const float padding = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 pad = {padding, padding};
    const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
    const float sectorWidth = std::max(width, HexContentWidth() + pad.x * 2.0f);
    const float innerWidth = sectorWidth - pad.x * 2.0f;

    float currentTop = -1.0f;
    float currentBottom = -1.0f;

    for (TrackSector& sector : m_trackSectors) {
        const bool isCurrent = sector.block == m_currentBlock;
        const int byteCount = isCurrent ? (int)m_data.size() : (int)sector.data.size();
        const int lines = (byteCount + bytesPerLine - 1) / bytesPerLine;
        const float hexHeight = lines * lineHeight;

        ImGui::PushID((int)sector.block);
        const float top = ImGui::GetCursorPosY();
        const ImVec2 screenTop = ImGui::GetCursorScreenPos();

        ImGui::Dummy(ImVec2(0.0f, pad.y));
        ImGui::Indent(pad.x);

        std::string header =
            SectorHeader(sector.block, isCurrent ? m_lastDetails.typeLabel : sector.typeLabel);
        // Without NoPadWithHalfSpacing the selected header's fill spreads into the padding
        if (ImGui::Selectable(header.c_str(), isCurrent, ImGuiSelectableFlags_NoPadWithHalfSpacing,
                              ImVec2(innerWidth, 0.0f)) &&
            !isCurrent) {
            m_pendingNavigate = true;
            m_pendingNavigateBlock = sector.block;
        }

        if (isCurrent) {
            DrawHexEditor(ImVec2(innerWidth, hexHeight), "##hex_sector", hexFlags);
        } else {
            sector.state.BytesPerLine = bytesPerLine;
            ImGui::PushStyleColor(ImGuiCol_Text, ColorManager::Instance().GetColor("Yellow"));
            PushFontRole(FontRole::Memory);
            ImGui::BeginHexEditor("##hex_sector", &sector.state, ImVec2(innerWidth, hexHeight), 0,
                                  hexFlags);
            ImGui::EndHexEditor();
            PopFontRole();
            ImGui::PopStyleColor();

            int hoveredByte = sector.state.HoveredByte;
            if (hoveredByte >= 0) {
                for (const auto& field : sector.fields) {
                    if (hoveredByte >= field.from && hoveredByte <= field.to) {
                        ImGui::SetTooltip("%s", field.name.c_str());
                        break;
                    }
                }
            }
        }

        ImGui::Unindent(pad.x);
        ImGui::Dummy(ImVec2(0.0f, pad.y));

        // The trailing item spacing is the gap below the sector, not part of it
        const ImVec2 screenBottom = {screenTop.x + sectorWidth,
                                     ImGui::GetCursorScreenPos().y - itemSpacingY};
        if (isCurrent) {
            currentTop = top;
            currentBottom = ImGui::GetCursorPosY();
            ImGui::GetWindowDrawList()->AddRect(screenTop, screenBottom,
                                                IM_COL32(255, 255, 255, 255));
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                   ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                   ImGui::IsMouseHoveringRect(screenTop, screenBottom)) {
            // A click anywhere in another sector makes it the current block
            m_pendingNavigate = true;
            m_pendingNavigateBlock = sector.block;
        }

        ImGui::PopID();
        // One blank line of text between sectors
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }

    // Bring the current block into view after it loads, unless it is already fully visible.
    // Kept for two frames: the first frame of a new layout has no scroll range yet.
    if (m_scrollToCurrentFrames > 0 && currentTop >= 0.0f) {
        --m_scrollToCurrentFrames;
        float scrollY = ImGui::GetScrollY();
        float viewHeight = ImGui::GetWindowHeight();
        if (currentTop < scrollY || currentBottom > scrollY + viewHeight)
            ImGui::SetScrollY(currentTop);
    }

    ImGui::EndChild();
}

void BlockEditorView::ShowFindDialog() { m_findDialogOpen = true; }

void BlockEditorView::FindNext() { PerformSearch(false); }

void BlockEditorView::FindPrevious() { PerformSearch(true); }

void BlockEditorView::Open() { m_isBlockOpen = true; }

void BlockEditorView::Restore()
{
    m_isBlockOpen = true;
    m_isBlockDetailsOpen = true;
    m_noFocusOnAppearing = true;
}

void BlockEditorView::Close() { m_isBlockOpen = false; }

void BlockEditorView::SetSearchContext(const std::vector<uint8_t>& pattern, bool caseSensitive,
                                       SearchType type)
{
    m_lastSearchType = type;
    m_isCaseSensitive = caseSensitive;

    // Update UI fields based on type
    if (type == SearchType::ASCII) {
        m_searchAscii = std::string(pattern.begin(), pattern.end());
    } else {
        // Convert pattern to hex string
        m_searchHex.clear();
        static const char hexChars[] = "0123456789ABCDEF";
        for (uint8_t byte : pattern) {
            m_searchHex += hexChars[(byte >> 4) & 0xF];
            m_searchHex += hexChars[byte & 0xF];
            m_searchHex += ' '; // Add space for readability
        }
        // Remove trailing space
        if (!m_searchHex.empty()) {
            m_searchHex.pop_back();
        }
    }
}

void BlockEditorView::HighlightSearchResult(uint32_t block, size_t offset, size_t length)
{
    m_hasSearchResult = true;
    m_searchResultBlock = block;
    m_searchResultOffset = offset;
    m_searchResultLength = length;

    GoToBlock(block);
}

} // namespace adm::ui
