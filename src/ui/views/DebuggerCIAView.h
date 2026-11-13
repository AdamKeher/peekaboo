#pragma once

#include <imgui.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "LockControls.h"
#include "RegisterDocRenderer.h"

namespace adm::ui {

class EmulatorView;

class DebuggerCIAView
{
public:
    explicit DebuggerCIAView(EmulatorView* emulatorView);

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    /// Hands the view the shared memory-lock store, which turns on the detail panel's Set/Lock
    /// controls and the per-row lock glyphs. Without it the view draws no lock UI at all.
    void SetLocks(LockUi ui) { m_lockUi = std::move(ui); }

private:
    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;

    // CIA register definitions
    nlohmann::json m_ciaRegsDef;
    bool m_ciaRegsDefLoaded = false;
    void LoadCIARegistersDef();

    struct RegisterRow
    {
        std::string address; // full hex address string e.g. "0xBFE001"
        std::string key;     // JSON key e.g. "BFE001"
        std::string name;
        uint8_t value;
        std::string rw;
        std::string description;
        std::string chip; // "ciaa" or "ciab"
        std::string docName;
        // Lowercased copies of key/name/chip, resolved once at build time so the per-frame filter
        // loop never re-lowercases or allocates.
        std::string keyLower;
        std::string nameLower;
        std::string chipLower;
    };
    std::vector<RegisterRow> m_regRows;
    // Like DebuggerCustomView: the rows are static metadata + a live value, so the rebuild (and its
    // JSON lookups + allocations) is gated on the emulator frame count rather than run every frame.
    uint64_t m_lastRowsFrame = UINT64_MAX;
    char m_regFilter[64] = "";
    bool m_showDocs = true;

    // Documentation panel
    nlohmann::json m_registerDocs;
    bool m_registerDocsLoaded = false;
    std::string m_selectedDocName;
    RegisterDocPanel m_docPanel{"##ciamdtbl"};
    std::string m_selectedRegisterName;
    float m_splitRatio = 0.5f;
    void LoadRegisterDocs();

    // The docs are grouped (one entry covers the same register on both chips), so the exact
    // selected row is tracked separately - Set and Lock act on one register, not on a group.
    std::string m_selectedKey; // e.g. "BFE001"; empty means nothing selected
    uint32_t m_selectedAddr = 0;
    uint32_t m_detailValue = 0;
    bool m_detailSync = true; // follow the selected register's live value

    // Shared lock store, plus the last Set/Lock outcome so a failed write or anchor resolve is
    // reported in the panel rather than swallowed.
    LockUi m_lockUi;
    std::string m_lockStatus;
    bool m_lockStatusIsError = false;

    void DrawContent();
    void DrawDetailPanel();
    void DrawDocPanel();
};

} // namespace adm::ui
