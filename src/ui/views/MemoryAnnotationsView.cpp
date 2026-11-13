#include "MemoryAnnotationsView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include "../AddressLink.h"

#include <imgui.h>

#include "../../core/MemoryAnnotations.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

namespace {

// Parses a hex address, tolerating an optional "$" or "0x" prefix. Empty/invalid
// input returns nullopt.
std::optional<uint32_t> ParseHexAddr(const char* s)
{
    if (!s)
        return std::nullopt;
    std::string t = s;
    // trim leading/trailing whitespace
    size_t b = t.find_first_not_of(" \t");
    if (b == std::string::npos)
        return std::nullopt;
    size_t e = t.find_last_not_of(" \t");
    t = t.substr(b, e - b + 1);
    if (t.rfind("$", 0) == 0)
        t.erase(0, 1);
    else if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0)
        t.erase(0, 2);
    if (t.empty())
        return std::nullopt;
    char* end = nullptr;
    unsigned long v = std::strtoul(t.c_str(), &end, 16);
    if (end == t.c_str() || *end != '\0')
        return std::nullopt;
    return static_cast<uint32_t>(v);
}

} // namespace

MemoryAnnotationsView::MemoryAnnotationsView(core::MemoryAnnotations* annotations)
    : m_annotations(annotations)
{
}

void MemoryAnnotationsView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

void MemoryAnnotationsView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (!ImGui::Begin(ICON_FA_TAG " Annotations###Annotations##memory", &m_open, flags)) {
        ImGui::End();
        return;
    }

    if (!m_annotations) {
        ImGui::TextDisabled("Annotation store unavailable.");
        ImGui::End();
        return;
    }

    // --- Add row -----------------------------------------------------------
    ImGui::TextUnformatted("Address");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    bool addrEnter = ImGui::InputText("##addaddr", m_addAddr, sizeof(m_addAddr),
                                      ImGuiInputTextFlags_CharsHexadecimal |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("##addtype", &m_addType, "Comment\0Label\0Block\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-70.0f);
    bool textEnter = ImGui::InputText("##addtext", m_addText, sizeof(m_addText),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool addClicked = ImGui::Button(ICON_FA_PLUS " Add");

    if (addClicked || addrEnter || textEnter) {
        auto addr = ParseHexAddr(m_addAddr);
        if (addr && m_addText[0] != '\0') {
            if (m_addType == 1)
                m_annotations->SetLabel(*addr, m_addText);
            else if (m_addType == 2)
                m_annotations->SetBlock(*addr, m_addText);
            else
                m_annotations->SetComment(*addr, m_addText);
            m_addAddr[0] = '\0';
            m_addText[0] = '\0';
        }
    }

    ImGui::Separator();

    // --- Table -------------------------------------------------------------
    const auto& all = m_annotations->All();
    if (all.empty()) {
        ImGui::TextDisabled("No annotations defined.");
        ImGui::End();
        return;
    }

    // Deferred mutations (avoid editing the map mid-iteration). A save tuple is
    // {address, field (0=comment/1=label), text}.
    std::optional<uint32_t> toDelete;
    std::optional<std::tuple<uint32_t, int, std::string>> toSave;

    // Renders one editable text cell. When this cell is the one being edited it
    // shows an input; otherwise the plain text plus a pencil to begin editing.
    auto drawCell = [&](uint32_t addr, int field, const std::string& text) {
        bool multiline = (field == 2); // comment blocks are multi-line
        bool editingThis = m_editing && m_editAddr == addr && m_editField == field;
        if (editingThis) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(field);
            bool commit = false;
            if (multiline) {
                // Enter inserts a newline; the edit commits when focus leaves the box.
                ImGui::InputTextMultiline("##edit", m_editText, sizeof(m_editText),
                                          ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f));
                commit = ImGui::IsItemDeactivatedAfterEdit();
            } else {
                commit = ImGui::InputText("##edit", m_editText, sizeof(m_editText),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            }
            ImGui::PopID();
            if (commit) {
                toSave = std::make_tuple(addr, field, std::string(m_editText));
                m_editing = false;
            }
        } else {
            ImGui::PushID(field);
            if (!text.empty())
                ImGui::TextUnformatted(text.c_str());
            else
                ImGui::TextDisabled("-");
            if (ImGui::IsItemClicked()) {
                m_editing = true;
                m_editAddr = addr;
                m_editField = field;
                std::strncpy(m_editText, text.c_str(), sizeof(m_editText) - 1);
                m_editText[sizeof(m_editText) - 1] = '\0';
            }
            ImGui::PopID();
        }
    };

    PushFontRole(FontRole::Memory);
    if (ImGui::BeginTable("##annotations_table", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingFixedFit | kAdjustableColumns |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                                30.0f);
        ImGui::TableHeadersRow();

        for (const auto& [addr, ann] : all) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(addr));

            // Address (clickable -> jump to memory, right-click -> shared actions menu)
            ImGui::TableSetColumnIndex(0);
            if (m_addrCtx && m_addrCtx->valid()) {
                DrawAddressLink("annot", addr, "$%08X", *m_addrCtx);
            } else {
                ImGui::Text("$%08X", addr);
            }

            // Label (field 1)
            ImGui::TableSetColumnIndex(1);
            drawCell(addr, 1, ann.label);

            // Comment (field 0)
            ImGui::TableSetColumnIndex(2);
            drawCell(addr, 0, ann.comment);

            // Block (field 2) — multi-line
            ImGui::TableSetColumnIndex(3);
            drawCell(addr, 2, ann.block);

            // Actions — trash removes the whole entry
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(ICON_FA_TRASH "##del"))
                toDelete = addr;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    PopFontRole();

    if (toSave) {
        auto [addr, field, text] = *toSave;
        if (field == 1)
            m_annotations->SetLabel(addr, text); // empty text clears the field
        else if (field == 2)
            m_annotations->SetBlock(addr, text);
        else
            m_annotations->SetComment(addr, text);
    }
    if (toDelete) {
        m_annotations->Remove(*toDelete);
        if (m_editing && m_editAddr == *toDelete)
            m_editing = false;
    }

    ImGui::End();
}

} // namespace adm::ui
