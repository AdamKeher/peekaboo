#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "../../core/RegWriteProfiler.h"
#include "AddressActions.h"

namespace adm::ui {

// "Register Operations" — every CPU and Copper write to a custom register this frame, in
// beam order. Answers "who wrote this register, and when", which neither the Copper trace
// (instructions, not effects) nor the Custom Registers window (current value, no history)
// can. Selecting a row syncs the whole debugger to that instant via its L-Ref.
//
// Adapted from the Chipset Log in the ENGINE9000 debugger by alpine9000, GPL-2 —
// see PATCHES.md. The L-Ref column and the cross-window time sync are additions.
class RegisterOpsView
{
public:
    explicit RegisterOpsView(adm::core::RegWriteProfiler* profiler);

    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void Draw();

    // Called when the scrubber moves — selects the last write at or before lRef.
    void SetHighlightLRef(uint32_t lRef);

    // Transient highlight from an external hover, independent of the click selection.
    // UINT32_MAX = clear.
    void SetHoverHighlightLRef(uint32_t lRef) { m_hoverHighlightLRef = lRef; }

    // Whether the loaded core exports the register-write log. Fed each frame so the window
    // can explain an empty table rather than looking broken.
    void SetCoreSupported(bool supported) { m_coreSupported = supported; }

    // Fired when the user clicks a row. Carries the source and PC so the caller can make the
    // jump source-aware: a copper write selects the MOVE in the Copper trace, a CPU write
    // navigates the disassembly to the writing instruction.
    std::function<void(uint32_t lRef, bool fromCopper, uint32_t pc)> m_onJumpToLRef;

    // Fired when the user hovers a row (UINT32_MAX = no row hovered).
    std::function<void(uint32_t lRef)> m_onHoverLRef;

    // Fired when the user clicks a register name — opens the Custom Registers window at that
    // offset with its bit-field decoder seeded from the value this row actually wrote.
    std::function<void(uint32_t offset, uint16_t value)> m_onJumpToCustomReg;

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    // Register name / description for an offset, resolved from assets/custom_registers.json.
    // Public so Application can build the headless snapshot from the same source the table shows.
    const std::string& RegisterName(uint16_t offset);
    const std::string& RegisterDescription(uint16_t offset);
    bool RegisterIsColor(uint16_t offset);

    // Filter state, persisted via UserPreferences.
    int GetSourceFilter() const { return m_sourceFilter; }
    void SetSourceFilter(int f) { m_sourceFilter = f; }
    bool GetChangedOnly() const { return m_changedOnly; }
    void SetChangedOnly(bool v) { m_changedOnly = v; }

private:
    adm::core::RegWriteProfiler* m_profiler = nullptr;
    const AddressActionContext* m_addrCtx = nullptr;

    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_coreSupported = true;

    // Selection state (indices into m_visible, the post-filter row list).
    int m_selectedIdx = -1;
    int m_lastSelectedIdx = -1;

    uint32_t m_lastHoveredLRef = UINT32_MAX;
    uint32_t m_highlightLRef = UINT32_MAX;
    uint32_t m_hoverHighlightLRef = UINT32_MAX;
    uint32_t m_lastHoverHighlightLRef = UINT32_MAX;

    // Suppresses hover-highlight auto-scroll while the mouse is over this window: our own
    // row-hover feeds the scrubber, which feeds a hover highlight straight back, and without
    // this guard the two fight over the scroll position.
    bool m_windowHovered = false;

    // Filters
    char m_nameFilter[64] = {};
    int m_sourceFilter = 0; // 0 = all, 1 = CPU only, 2 = Copper only
    bool m_changedOnly = false;
    int m_lineFrom = 0;
    int m_lineTo = 312;

    // Indices into the profiler's frame that survive the current filter. Rebuilt each frame;
    // all row/selection indices in this view refer to positions in here.
    std::vector<int> m_visible;

    void DrawToolbar(const adm::core::RegWriteProfiler::FrameStats& stats);
    void DrawTable(const std::vector<adm::core::RegWriteRecord>& records);
    void RebuildVisible(const std::vector<adm::core::RegWriteRecord>& records);
    bool PassesFilter(const adm::core::RegWriteRecord& rec);

    // Lazily-loaded custom-register definitions (the same JSON the Copper and Custom
    // Registers windows use).
    nlohmann::json m_regsDef;
    bool m_regsDefLoaded = false;
    void LoadRegsDef();

    // Resolved-name cache, indexed by offset>>1, so the table doesn't re-query the JSON for
    // every visible row every frame.
    struct RegInfo
    {
        bool resolved = false;
        bool isColor = false;
        std::string name;
        std::string description;
    };
    static constexpr int kRegSlots = 256;
    RegInfo m_regInfo[kRegSlots];
    RegInfo& Info(uint16_t offset);
};

} // namespace adm::ui
