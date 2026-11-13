#pragma once

#include <imgui.h>

#include <cctype>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "imgui_md.h"

namespace adm::ui {

// Shared markdown renderer for the register-documentation panels (Custom / CIA / Copper views).
// Renders GitHub-style pipe tables as native ImGui tables and turns "#anchor" links into in-panel
// doc navigation by writing the lowercased anchor into *targetDocName.
//
// Each panel keeps its own static instance and passes a distinct `tableIdPrefix` so the generated
// table ids stay unique.
struct RegisterDocRenderer : public imgui_md
{
    std::string* targetDocName = nullptr;
    const char* tableIdPrefix = "##mdtbl";

    bool m_in_native_table = false;
    bool m_in_thead = false;
    int m_native_table_id = 0;

    explicit RegisterDocRenderer(const char* idPrefix = "##mdtbl") : tableIdPrefix(idPrefix) {}

    void open_url() const override
    {
        if (!m_href.empty() && m_href[0] == '#' && targetDocName) {
            std::string anchor = m_href.substr(1);
            for (auto& c : anchor)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            *targetDocName = anchor;
        }
    }

    void BLOCK_DOC(bool e) override
    {
        if (e)
            m_native_table_id = 0;
        imgui_md::BLOCK_DOC(e);
    }

    void BLOCK_TABLE(const MD_BLOCK_TABLE_DETAIL* d, bool e) override
    {
        if (e) {
            ImGui::Spacing();
            int cols = d ? (int)d->col_count : 3;
            char id[40];
            snprintf(id, sizeof(id), "%s%d", tableIdPrefix, m_native_table_id++);

            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;

            m_in_native_table = ImGui::BeginTable(id, cols, flags);
        } else {
            if (m_in_native_table) {
                ImGui::EndTable();
                m_in_native_table = false;
            }
        }
    }

    void BLOCK_THEAD(bool e) override
    {
        m_is_table_header = e;
        m_in_thead = e;
        if (m_table_header_highlight) {
            if (e)
                ImGui::PushFont(get_font());
            else
                ImGui::PopFont();
        }
    }

    void BLOCK_TBODY(bool e) override
    {
        if (m_in_native_table)
            m_is_table_header = e;
    }

    void BLOCK_TR(bool e) override
    {
        if (!m_in_native_table)
            return;
        if (e) {
            ImGui::TableNextRow();
            if (m_in_thead) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 60, 80, 128));
            }
        }
    }

    void BLOCK_TH(const MD_BLOCK_TD_DETAIL*, bool e) override
    {
        if (!m_in_native_table)
            return;
        if (e)
            ImGui::TableNextColumn();
    }

    void BLOCK_TD(const MD_BLOCK_TD_DETAIL*, bool e) override
    {
        if (!m_in_native_table)
            return;
        if (e)
            ImGui::TableNextColumn();
    }
};

// One register-doc panel: owns the renderer plus a memo of the extracted markdown text.
// Pulling the string out of the JSON every frame allocated the whole document per frame, and
// the fix had only been applied to one of the three copies of this code — hence the shared
// helper. A member (not a function-local static) so multiple view instances don't share state.
struct RegisterDocPanel
{
    explicit RegisterDocPanel(const char* idPrefix) : renderer(idPrefix) {}

    // Caller guarantees docs.contains(selectedDocName). selectedDocName is the view's member so
    // "#anchor" links can retarget it mid-print (the re-extract happens on the next frame).
    void Draw(const nlohmann::json& docs, std::string& selectedDocName)
    {
        if (m_cachedName != selectedDocName) {
            m_cachedName = selectedDocName;
            m_cachedText = docs.at(selectedDocName).get<std::string>();
        }
        renderer.targetDocName = &selectedDocName;
        renderer.print(m_cachedText.c_str(), m_cachedText.c_str() + m_cachedText.size());
    }

    RegisterDocRenderer renderer;

private:
    std::string m_cachedName;
    std::string m_cachedText;
};

} // namespace adm::ui
