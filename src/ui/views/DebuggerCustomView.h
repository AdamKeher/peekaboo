#pragma once

#include <imgui.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "AddressActions.h"
#include "LockControls.h"
#include "RegisterDocRenderer.h"

namespace adm::ui {

class EmulatorView;

class DebuggerCustomView
{
public:
    explicit DebuggerCustomView(EmulatorView* emulatorView);

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    // Open the Custom Registers window and scroll/select the register at `offset`. When `hasValue`
    // is set, the bit-field decoder is seeded with `value` (e.g. a copperlist MOVE's word2) instead
    // of the live register value.
    void NavigateToRegister(uint32_t offset, bool hasValue = false, uint32_t value = 0);

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    /// Hands the view the shared memory-lock store, which turns on the decoder's Set/Lock
    /// controls and the per-row lock glyphs. Without it the view draws no lock UI at all.
    void SetLocks(LockUi ui) { m_lockUi = std::move(ui); }

private:
    EmulatorView* m_emulatorView;
    const AddressActionContext* m_addrCtx = nullptr;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    // Frames remaining to re-assert window focus. A docked tab bar tracks g.NavWindow every frame,
    // so a single SetNextWindowFocus can lose a focus race against the click that triggered the
    // navigation when the source window shares this dock node — re-assert for a few frames.
    int m_focusFrames = 0;
    int m_pendingNavigateOffset = -1;

    // Value to seed the decoder with after navigation (e.g. a copperlist MOVE's word2), independent
    // of the live register value. Applied once on the frame the navigated register becomes
    // selected.
    bool m_hasPendingDecoderValue = false;
    uint32_t m_pendingDecoderValue = 0;

    // Custom register definitions
    nlohmann::json m_customRegsDef;
    bool m_customRegsDefLoaded = false;
    void LoadCustomRegistersDef();

    // Bring ancestor host panels forward so a nested-docked Custom Registers window actually
    // becomes visible when focused (see implementation for the docking details).
    void FocusContainingPanels();

    struct RegisterRow
    {
        int offset;
        std::string name;
        uint32_t value;
        int bitSize;
        std::string rw;
        std::string description;
        bool isColor;
        uint32_t displayValue;
        bool isAddress = false;
        std::string address;
        std::string chipset;
        // JSON-derived doc group + a lowercased name, resolved once at row-build time so the
        // per-frame render/filter loops never touch the JSON map or allocate.
        std::string docName;
        std::string nameLower;
    };
    std::vector<RegisterRow> m_customRegRows;
    // offset -> index into m_customRegRows, for value-only refreshes after the table is sorted.
    std::unordered_map<int, size_t> m_rowIndex;
    bool m_rowIndexDirty = true;
    // m_customRegRows holds static (JSON-derived) metadata + the live register value. Rebuilding it
    // — and its ~250 JSON lookups + string allocations — only matters when the emulator advances a
    // frame, so gate the rebuild on the emulator frame count (like Copper/Bitplanes/Sprites). While
    // paused the cached rows stay valid and the per-frame cost drops to sort + render only.
    uint64_t m_lastRowsFrame = UINT64_MAX;
    char m_customRegFilter[64] = "";
    bool m_showCustomDocs = true;
    bool m_showDecoder = true;

    // Register docs panel
    nlohmann::json m_registerDocs;
    bool m_registerDocsLoaded = false;
    std::string m_selectedDocName;
    std::string m_selectedRegisterName;
    RegisterDocPanel m_docPanel{"##mdtbl"};
    int m_selectedOffset = -1; // exact register selected (registers can share a doc_name group)
    float m_customSplitRatio = 0.5f; // horizontal split: table+decoder / docs
    float m_vertSplitRatio = 0.62f;  // vertical split: register table / decoder
    void LoadRegisterDocs();

    void DrawContent();
    void DrawDocPanel();

    // -- Embedded bit-field decoder / value calculator (right pane) ---------------------------
    struct DecoderField
    {
        int hi = 0;
        int lo = 0;
        std::string name;
        std::string desc;
    };

    nlohmann::json m_bitfieldDefs;
    bool m_bitfieldDefsLoaded = false;
    void LoadBitfieldDefs();

    std::string m_decoderDocName;
    std::string m_decoderRegName;
    int m_decoderBits = 16;
    uint32_t m_decoderValue = 0;
    bool m_decoderSync = true;    // follow the selected register's live value
    int64_t m_lastSyncValue = -1; // de-dup so user edits persist until the live value changes

    // Field definitions. Most registers have a single layout; some (BLTCON0/BLTCON1) carry several
    // mode variants (Area / Line) the user picks between. m_decoderModes is empty/size-1 for a
    // plain register and >1 when a mode selector should be shown.
    std::vector<std::string> m_decoderModes;
    // Shared lock store, plus the last Set/Lock outcome so a failed anchor resolve is reported
    // in the panel rather than swallowed.
    LockUi m_lockUi;
    std::string m_lockStatus;
    bool m_lockStatusIsError = false;
    /// Writes m_decoderValue to the selected register (two word writes for a 32-bit pointer).
    bool WriteDecoderValueToChipset();
    /// The chipset address the decoder's value lives at, or 0 when nothing is selected.
    uint32_t DecoderAddress() const;
    void DrawDecoderLockControls();
    std::vector<std::vector<DecoderField>> m_decoderModeFields;
    int m_decoderMode = 0;
    const std::vector<DecoderField>& ActiveFields() const;

    void SetDecoderFields(const std::string& regName, const std::string& docName);
    void UpdateDecoder(const std::string& docName, const std::string& regName, uint32_t value,
                       int bits, bool valueValid);
    void DrawDecoderPanel();
    uint32_t FieldValue(const DecoderField& f) const;
    void SetFieldValue(const DecoderField& f, uint32_t v);
};

} // namespace adm::ui
