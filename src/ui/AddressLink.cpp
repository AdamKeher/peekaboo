#include "AddressLink.h"

#include <cstdio>

#include <fmt/format.h>
#include <imgui.h>

#include "../core/MemoryAnnotations.h"
#include "HyperlinkText.h"
#include "views/AddressMenuActions.h"

namespace adm::ui {

namespace {

// Global state for the label/comment editor modal. Only one editor is ever open at a time, so
// file-static state (rather than per-link) is enough and keeps the editor alive after the
// originating context menu closes.
struct AnnotEditState
{
    bool open = false;    // request to open the modal this frame
    bool isLabel = false; // editing the label (vs. the comment)
    uint32_t addr = 0;
    core::MemoryAnnotations* annotations = nullptr;
    char buf[256] = {};
};

AnnotEditState g_annotEdit;

void OpenAnnotEditor(uint32_t addr, bool isLabel, core::MemoryAnnotations* annotations,
                     const char* current)
{
    g_annotEdit.open = true;
    g_annotEdit.isLabel = isLabel;
    g_annotEdit.addr = addr;
    g_annotEdit.annotations = annotations;
    std::snprintf(g_annotEdit.buf, sizeof(g_annotEdit.buf), "%s", current ? current : "");
}

} // namespace

void RunAddressDefaultAction(uint32_t addr, const AddressActionContext& ctx, AddressLinkHost host,
                             AddressDefault def)
{
    switch (def) {
        case AddressDefault::Memory:
            if (ctx.jumpToMemory)
                ctx.jumpToMemory(addr, 1);
            break;
        case AddressDefault::Disasm:
            if (ctx.jumpToDisasm)
                ctx.jumpToDisasm(addr);
            break;
        case AddressDefault::MemoryMap:
            if (ctx.showInMemoryMap)
                ctx.showInMemoryMap(addr);
            break;
        case AddressDefault::FollowPointer: {
            uint32_t v = 0;
            if (ctx.readLong && ctx.readLong(addr, v) && ctx.jumpToMemory)
                ctx.jumpToMemory(v & 0x00FFFFFFu, 1);
            break;
        }
    }
    (void)host;
}

void DrawAddressMenuBody(uint32_t addr, const AddressActionContext& ctx, AddressLinkHost host,
                         AddressDefault def)
{
    ImGui::TextDisabled("$%06X", addr);
    ImGui::Separator();

    // Breakpoint toggle + Read/Write/R+W watchpoint submenu (shared with the disassembly view).
    DrawBreakpointMenuItems(ctx.breakpoints, addr);

    // Annotation — label and comment are independent fields.
    if (ctx.annotations) {
        auto label = ctx.annotations->GetLabel(addr);
        if (ImGui::MenuItem(label ? "Edit label..." : "Set label..."))
            OpenAnnotEditor(addr, true, ctx.annotations, label ? label->c_str() : nullptr);
        if (label && ImGui::MenuItem("Clear label"))
            ctx.annotations->RemoveLabel(addr);

        auto comment = ctx.annotations->GetComment(addr);
        if (ImGui::MenuItem(comment ? "Edit comment..." : "Set comment..."))
            OpenAnnotEditor(addr, false, ctx.annotations, comment ? comment->c_str() : nullptr);
        if (comment && ImGui::MenuItem("Clear comment"))
            ctx.annotations->RemoveComment(addr);
    }

    ImGui::Separator();

    // Memory dump — console host prints inline; everywhere else opens the Memory window.
    if (ImGui::MenuItem("Memory dump")) {
        if (host == AddressLinkHost::Console && ctx.runConsoleCommand)
            ctx.runConsoleCommand(fmt::format("MM {:X}", addr));
        else if (ctx.jumpToMemory)
            ctx.jumpToMemory(addr, 1);
    }

    // Disassemble — console host prints inline; everywhere else opens the Disassembly window.
    if (ImGui::MenuItem("Disassemble")) {
        if (host == AddressLinkHost::Console && ctx.runConsoleCommand)
            ctx.runConsoleCommand(fmt::format("DD {:X}", addr));
        else if (ctx.jumpToDisasm)
            ctx.jumpToDisasm(addr);
    }

    if (ImGui::MenuItem("Memory Map") && ctx.showInMemoryMap)
        ctx.showInMemoryMap(addr);

    ImGui::Separator();

    if (ImGui::MenuItem("Copy address")) {
        char b[16];
        std::snprintf(b, sizeof(b), "$%06X", addr);
        ImGui::SetClipboardText(b);
    }

    // Follow pointer — read the long at addr and jump to it (always via the default action).
    if (ImGui::MenuItem("Follow pointer")) {
        uint32_t v = 0;
        if (ctx.readLong && ctx.readLong(addr, v))
            RunAddressDefaultAction(v & 0x00FFFFFFu, ctx, host, AddressDefault::Memory);
    }

    // Run to here — temporary breakpoint, removed when it fires (UAE TA command).
    if (ImGui::MenuItem("Run to here") && ctx.runConsoleCommand)
        ctx.runConsoleCommand(fmt::format("TA {:X}", addr));

    // Set PC to here — only meaningful while paused.
    const bool paused = ctx.isPaused && ctx.isPaused();
    if (!paused)
        ImGui::BeginDisabled();
    if (ImGui::MenuItem("Set PC to here") && ctx.runConsoleCommand)
        ctx.runConsoleCommand(fmt::format("R PC {:X}", addr));
    if (!paused)
        ImGui::EndDisabled();

    (void)def;
}

void DrawAddressLink(const char* idSuffix, uint32_t addr, const char* fmt,
                     const AddressActionContext& ctx, AddressLinkHost host, AddressDefault def)
{
    char text[32];
    std::snprintf(text, sizeof(text), fmt, addr);
    ImGui::TextColored(kHyperlinkColor, "%s", text);

    bool clicked = false;
    const bool hovered = HoverableLink(&clicked);

    char popupId[64];
    std::snprintf(popupId, sizeof(popupId), "##aladdr_%s_%X", idSuffix, addr);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup(popupId);
    if (clicked)
        RunAddressDefaultAction(addr, ctx, host, def);

    if (ImGui::BeginPopup(popupId)) {
        DrawAddressMenuBody(addr, ctx, host, def);
        ImGui::EndPopup();
    }
}

void DrawAddressLinkModals()
{
    if (g_annotEdit.open) {
        ImGui::OpenPopup("##al_annot_edit");
        g_annotEdit.open = false;
    }
    if (ImGui::BeginPopup("##al_annot_edit")) {
        ImGui::Text("%s for $%06X", g_annotEdit.isLabel ? "Label" : "Comment", g_annotEdit.addr);
        ImGui::SetNextItemWidth(280);
        const bool commit = ImGui::InputText("##remark", g_annotEdit.buf, sizeof(g_annotEdit.buf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || commit) {
            // An empty buffer clears that one field (SetLabel/SetComment handle it).
            if (g_annotEdit.annotations) {
                if (g_annotEdit.isLabel)
                    g_annotEdit.annotations->SetLabel(g_annotEdit.addr, g_annotEdit.buf);
                else
                    g_annotEdit.annotations->SetComment(g_annotEdit.addr, g_annotEdit.buf);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace adm::ui
