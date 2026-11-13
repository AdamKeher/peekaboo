#pragma once

#include <imgui.h>
#include <imgui_hex.h>
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include "../../core/AmigaEmulator.h"
#include "../../core/DiskSearcher.h"
#include "../../core/MemorySearcher.h"
#include "LockControls.h"

namespace adm::ui {

class EmulatorView;

class DebuggerMemoryView
{
public:
    explicit DebuggerMemoryView(EmulatorView* emulatorView);

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    // Current base address / navigate to one — used by the .peek session format to save and
    // restore the memory view position.
    uint32_t GetCurrentAddress() const { return m_selectedAmigaBase; }

    // Bytes-per-line (hex columns) — persisted across sessions via user prefs.
    int GetBytesPerLine() const
    {
        return m_memHexState.BytesPerLine > 0 ? m_memHexState.BytesPerLine : 16;
    }
    void SetBytesPerLine(int bpl)
    {
        if (bpl > 0)
            m_memHexState.BytesPerLine = bpl;
    }
    // focus=false navigates without grabbing window focus — used by .peek restore so the
    // saved layout's active window is preserved instead of being yanked to the Memory view.
    void GoToAddress(uint32_t amigaAddr, bool focus = true)
    {
        HighlightMemoryResult(amigaAddr, 1, focus);
    }

    // External entry points
    void HighlightMemoryResult(uint32_t amigaAddr, size_t length, bool focus = true);
    void MarkRegionsDirty();
    void SetMemorySearcher(core::MemorySearcher* searcher);
    void ShowFindDialog();
    void FindNext();
    void FindPrevious();
    void SetOnMemoryTabActive(std::function<void()> callback);

    /// Hands the view the shared memory-lock store, which turns on the locked-range highlights
    /// and the hex editor's Lock/Set/Remove context menu. Without it none of that is drawn.
    void SetLocks(LockUi ui) { m_lockUi = std::move(ui); }

private:
    // Amiga Exec memory attribute flags
    static constexpr uint16_t kMemF_Chip = 0x0002;
    static constexpr uint16_t kMemF_Fast = 0x0004;

    struct ParsedMemChunk
    {
        uint32_t address;
        uint32_t size;
        bool isFree;
        uint16_t attributes;

        bool operator<(const ParsedMemChunk& other) const { return address < other.address; }
    };

    struct ParsedMemHeader
    {
        uint32_t headerAddress;
        uint16_t attributes;
        uint32_t lower;
        uint32_t upper;
        uint32_t freeSpace;

        std::vector<ParsedMemChunk> chunks;
    };
    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    bool m_shouldFocus = false;

    std::vector<adm::AmigaEmulator::MemoryRegion> m_cachedMemRegions;
    bool m_memRegionsDirty = true;
    int m_resetPendingCount = 0;

    int m_selectedRegionIdx = -1;
    uint32_t m_selectedAmigaBase = 0;
    std::vector<uint8_t> m_memSnapshot;
    ImGuiHexEditorState m_memHexState;
    bool m_memHexStateValid = false;

    core::MemorySearcher* m_memorySearcher = nullptr;
    bool m_hasMemSearchResult = false;
    uint32_t m_memSearchResultAddr = 0;
    size_t m_memSearchResultLength = 0;
    int m_memScrollToOffset = -1;

    bool m_findMemDialogOpen = false;
    bool m_focusMemFindInput = false;
    core::DiskSearcher::SearchType m_lastMemSearchType = core::DiskSearcher::SearchType::ASCII;
    bool m_isMemCaseSensitive = false;
    std::string m_memSearchAscii;
    std::string m_memSearchHex;

    bool m_wasMemoryFocused = false;
    std::function<void()> m_onMemoryTabActive;

    // Memory locks. Highlights are opt-out rather than opt-in: a locked range that is silently
    // invisible is exactly the surprise this is meant to prevent.
    LockUi m_lockUi;
    bool m_showLocks = true;
    // "Set value" needs a value before it can make a lock, so the context menu stages the
    // selection here and a modal collects the rest on the following frames.
    bool m_openSetValuePopup = false;
    uint32_t m_lockDialogAddr = 0;
    uint32_t m_lockDialogValue = 0;
    int m_lockDialogSize = 1; // bytes: 1, 2 or 4
    std::string m_lockStatus;
    bool m_lockStatusIsError = false;

    /// Right-click items appended to the hex editor's own menu (Copy stays first).
    void DrawHexContextMenu();
    void DrawSetValueDialog();
    /// Bytes currently selected in the hex editor, as an absolute Amiga range. Returns false
    /// when nothing is selected.
    bool SelectedRange(uint32_t& addr, uint32_t& len) const;

    void SelectMemoryRegionInternal(int regionIdx);
    void PerformMemorySearch(bool reverse);
    void DrawContent();
    void DrawToolbar();

    // Memory Stats Tab Methods
    void DrawStatsTab();
    void RefreshAllocations();
    bool Read32(uint32_t amigaAddr, uint32_t& out_val) const;
    bool Read16(uint32_t amigaAddr, uint16_t& out_val) const;

    bool m_memAllocationsDirty = true;
    std::chrono::steady_clock::time_point m_lastMemAllocRefresh;
    uint32_t m_parsedExecBase = 0;
    uint32_t m_parsedMemList = 0;
    std::vector<ParsedMemHeader> m_parsedMemHeaders;
    bool m_forceInspectorTab = false;
};

} // namespace adm::ui
