#pragma once

#include <imgui.h>
#include <functional>
#include <string>

#include <fmt/format.h>

#include "../../core/MemoryLocks.h"
#include "../IconsFontAwesome6.h"

namespace adm::ui {

// Everything a register window or the memory inspector needs to show and toggle a memory lock.
//
// The store is shared - it is the same core::MemoryLocks the PR console command and the Trainer
// window drive - so a lock made from the Custom decoder shows up in `PR` and in the Trainer
// table with no synchronisation code. Views hold this by value and default-construct it, so a
// view whose owner never called SetLocks simply draws no lock UI rather than crashing.
struct LockUi
{
    core::MemoryLocks* locks = nullptr;
    std::function<core::LockContext()> makeContext;

    bool Valid() const { return locks != nullptr; }

    core::LockContext Context() const { return makeContext ? makeContext() : core::LockContext{}; }

    /// The lock covering `addr`, or null. Only ever finds resolved entries.
    const core::LockEntry* At(uint32_t addr) const
    {
        return locks ? locks->FindCovering(addr) : nullptr;
    }
};

// Block is red and Set is amber throughout - the memory inspector's borders, the register
// windows' glyphs and the Trainer table all use these two, so a colour means one thing.
inline ImVec4 LockModeColor(core::LockMode m)
{
    switch (m) {
        case core::LockMode::Block:
            return ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
        case core::LockMode::Set:
            return ImVec4(0.95f, 0.72f, 0.25f, 1.0f);
        case core::LockMode::Patch:
            return ImVec4(0.55f, 0.75f, 0.95f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

/// A lock glyph for a table row, with a tooltip naming the entry. Draws nothing when `e` is null,
/// so row loops can call it unconditionally.
inline void DrawLockGlyph(const core::LockEntry* e)
{
    if (!e)
        return;
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(LockModeColor(e->mode), ICON_FA_LOCK);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s lock %s%s\n%s", core::ToString(e->mode), e->id.c_str(),
                          e->enabled ? "" : " (disabled)", e->desc.c_str());
    }
}

/// Adds a Set-mode lock at `addr` and enables it. Returns false with `err` filled when the
/// anchor does not resolve or its guard bytes mismatch - in which case the entry is still added
/// (so the user can see and fix it) but stays disabled and nothing was written.
inline bool AddSetLock(const LockUi& ui, uint32_t addr, uint8_t size, uint32_t value,
                       std::string desc, std::string& err)
{
    if (!ui.Valid()) {
        err = "memory locks unavailable";
        return false;
    }
    core::LockEntry e;
    e.anchor.kind = core::AnchorKind::Absolute;
    e.anchor.addr = addr;
    e.mode = core::LockMode::Set;
    e.size = size;
    e.len = size; // A forced value protects exactly its own width.
    e.value = value;
    e.desc = desc.empty() ? fmt::format("set ${:08X}", addr) : std::move(desc);
    const std::string id = ui.locks->Add(e);
    return ui.locks->SetEnabled(id, true, ui.Context(), err);
}

/// Adds a Block-mode lock over `len` bytes at `addr` and enables it. Same failure contract as
/// AddSetLock.
inline bool AddBlockLock(const LockUi& ui, uint32_t addr, uint32_t len, std::string desc,
                         std::string& err)
{
    if (!ui.Valid()) {
        err = "memory locks unavailable";
        return false;
    }
    core::LockEntry e;
    e.anchor.kind = core::AnchorKind::Absolute;
    e.anchor.addr = addr;
    e.mode = core::LockMode::Block;
    e.len = len < 1 ? 1 : len;
    e.desc = desc.empty() ? fmt::format("block ${:08X}", addr) : std::move(desc);
    const std::string id = ui.locks->Add(e);
    return ui.locks->SetEnabled(id, true, ui.Context(), err);
}

/// Removes every lock covering `addr`, reverting any applied patch first (the same order
/// `PR DEL` uses - a patch dropped while still applied would leave its bytes in memory).
inline size_t RemoveLocksAt(const LockUi& ui, uint32_t addr)
{
    if (!ui.Valid())
        return 0;
    const core::LockContext ctx = ui.Context();
    for (const auto& e : ui.locks->All()) {
        if (e.mode == core::LockMode::Patch && e.applied && e.resolved && addr >= *e.resolved &&
            addr - *e.resolved < e.Span()) {
            std::string err;
            ui.locks->SetEnabled(e.id, false, ctx, err);
        }
    }
    return ui.locks->RemoveCovering(addr);
}

} // namespace adm::ui
