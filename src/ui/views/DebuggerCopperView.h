#pragma once

#include <imgui.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "../../core/AmigaEmulator.h"
#include "../../core/Constants.h"
#include "../../core/CopperDisassembler.h"
#include "AddressActions.h"
#include "RegisterDocRenderer.h"

namespace adm::ui {

class EmulatorView;
class BreakpointsView;

class DebuggerCopperView
{
public:
    explicit DebuggerCopperView(EmulatorView* emulatorView);

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    // Wired after construction (the breakpoint store is built later). Enables the gutter that
    // toggles a Copper breakpoint (Read/COP watchpoint) on an instruction's address.
    void SetBreakpointsView(BreakpointsView* bv) { m_breakpointsView = bv; }

    // Set by DebuggerFrameScrubberView — scroll/highlight to instruction at this L-Ref
    void SetScrubberLRef(uint32_t lRef) { m_scrubberLRef = lRef; }

    // Open the window, switch to Trace mode, then select/scroll to the instruction at this L-Ref.
    // Used when a copper tick is clicked in the Frame Scrubber.
    void JumpToTraceLRef(uint32_t lRef);

    // Fired when user clicks an L-Ref cell — outbound to frame scrubber
    std::function<void(uint32_t lRef)> m_onJumpToLRef;

    // Fired when user hovers a trace row — UINT32_MAX when no row is hovered
    std::function<void(uint32_t lRef)> m_onHoverLRef;

    // Fired when user clicks a MOVE register name — opens the Custom Registers window at that
    // offset and seeds its bit-field decoder with the instruction's value (word2).
    std::function<void(uint32_t offset, uint16_t value)> m_onJumpToCustomReg;

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

private:
    enum class Mode { Live, Trace };

    EmulatorView* m_emulatorView;
    const AddressActionContext* m_addrCtx = nullptr;
    BreakpointsView* m_breakpointsView = nullptr;

    // Draw the Copper-breakpoint gutter cell for an instruction at `addr` (red dot + click).
    void DrawCopperBreakpointGutter(uint32_t addr, int rowId);
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    bool m_requestFocus = false; // bring the window to front on the next Draw()
    Mode m_mode = Mode::Live;

    nlohmann::json m_customRegsDef;
    bool m_customRegsDefLoaded = false;
    void LoadCustomRegistersDef();

    nlohmann::json m_registerDocs;
    bool m_registerDocsLoaded = false;
    RegisterDocPanel m_docPanel{"##mdcpytbl"};
    void LoadRegisterDocs();
    void DrawDocPanel();

    // Bit-field layouts (assets/custom_registers_bitfields.json), keyed by doc_name. Used to render
    // the hover popup that breaks a MOVE's value down into named bit fields.
    nlohmann::json m_bitfieldDefs;
    bool m_bitfieldDefsLoaded = false;
    void LoadBitfieldDefs();

    // Hover popup: decode `value` against the bit-field layout for `regName`/`docName` and show
    // each field's bit range, value, and name. No-op when the register has no bit-field descriptor.
    void DrawBitfieldTooltip(const std::string& regName, const std::string& docName,
                             uint16_t value);

    bool m_showCopperDocs = false;
    bool m_showDecoder = true;
    float m_copperSplitRatio = 0.6f;
    float m_decoderSplitRatio = 0.68f;
    std::string m_selectedDocName;
    std::string m_selectedRegisterName;

    // Embedded bit-field decoder / value calculator. The selected Copper MOVE supplies the
    // register and value; Sync follows changes to that instruction while still allowing the
    // decoder to be used as a calculator when disabled.
    struct DecoderField
    {
        int hi = 0;
        int lo = 0;
        std::string name;
        std::string desc;
    };

    std::string m_decoderDocName;
    std::string m_decoderRegName;
    uint32_t m_decoderValue = 0;
    bool m_decoderSync = true;
    int64_t m_lastSyncValue = -1;
    std::vector<std::string> m_decoderModes;
    std::vector<std::vector<DecoderField>> m_decoderModeFields;
    int m_decoderMode = 0;
    uint32_t m_selectedLiveInstructionAddr = UINT32_MAX;
    int m_selectedDecoderTraceIdx = -1;

    const std::vector<DecoderField>& ActiveDecoderFields() const;
    void SetDecoderFields(const std::string& regName, const std::string& docName);
    void UpdateDecoder(const std::string& docName, const std::string& regName, uint16_t value);
    void SelectMoveInstruction(uint16_t word1, uint16_t word2, uint32_t addr, int traceIdx);
    void DrawDecoderPanel();
    uint32_t DecoderFieldValue(const DecoderField& field) const;
    void SetDecoderFieldValue(const DecoderField& field, uint32_t value);

    uint32_t m_copperDumpAddr = 0;
    int m_copperListSource = 0;
    char m_copperCustomAddrStr[16] = "";

    // Execution log built from copper exec events each frame
    std::vector<adm::AmigaEmulator::CopperExecEvent> m_executionLog;
    bool m_execOverflow = false;

    // Writes to COP1LC/COP2LC/COPJMP1/COPJMP2 seen during the frame, harvested from the
    // register-write log so that CPU-side changes — invisible in the copper's own trace — are
    // accounted for too. Ordered by L-Ref, same as the execution log.
    struct PointerWrite
    {
        uint32_t lRef = 0;
        uint16_t reg = 0;   // 0x080..0x08A
        uint16_t value = 0; // word written
        uint32_t pc = 0;    // writing CPU instruction, or the copper MOVE's address
        bool fromCopper = false;
    };
    std::vector<PointerWrite> m_pointerWrites;
    std::vector<adm::AmigaEmulator::RegWriteEvent> m_regWriteScratch;

    // Set when the list the copper actually ran this frame is not the one the Live tab would
    // decode from the current COP1LC/COP2LC — i.e. the pointers moved after the copper started.
    bool m_midFrameRewrite = false;
    std::string m_midFrameSummary; // tooltip body for the badge
    int m_firstPointerRow = -1;    // first synthetic row in m_formattedLog, -1 if none

    // Rebuild gate: the copper trace + its formatted rows (a string-heavy per-event reformat) only
    // change when the emulator advances a frame, so we skip the rebuild while paused.
    uint64_t m_lastRebuildEmuFrame = UINT64_MAX;

    // Scratch for the exec-event fetch (was a function-local static shared across instances).
    std::vector<adm::AmigaEmulator::CopperExecEvent> m_eventScratch;

    // Region list refreshed only when the bank-layout signature changes.
    std::vector<adm::AmigaEmulator::MemoryRegion> m_regions;
    uint64_t m_regionsSig = UINT64_MAX;

    // Live-list pre-scan cache, keyed on (start address, emulated frame) — see the Live tab.
    struct LiveRow
    {
        uint32_t addr;
        adm::core::CopperInstruction inst;
        uint16_t word1;
    };
    std::vector<LiveRow> m_liveRows;
    uint64_t m_liveRowsFrame = UINT64_MAX;
    uint32_t m_liveRowsAddr = UINT32_MAX;

    // Pre-formatted log rows (rebuilt whenever m_executionLog changes). The list interleaves the
    // copper's own instructions with synthetic rows for CPU writes that move a copper pointer, so
    // it is longer than m_executionLog — index it, not the execution log, from the UI.
    enum class RowKind { Instruction, PointerWrite };

    struct FormattedCopperRow
    {
        RowKind kind = RowKind::Instruction;
        int execIdx = -1;      // index into m_executionLog, -1 for a synthetic pointer row
        std::string lRefLabel; // "L%07X##ctr%d" — selectable label
        std::string listStr;   // "COP1", "COP2", or "???"
        std::string numStr;    // row number e.g. "42"
        std::string addrStr;   // "$%08X"
        std::string wordsStr;  // inst.formattedWords
        std::string instrName; // inst.name
        std::string details;   // inst.details
        adm::core::CopperInstruction::Type instrType = adm::core::CopperInstruction::Type::NOOP;
        bool isColor = false;
        float r = 0, g = 0, b = 0;
        uint16_t word1 = 0;
        uint16_t word2 = 0;
        uint32_t addr = 0;
        uint32_t lRefValue = 0;
        uint32_t vline = 0, hclk = 0;

        // Pointer rows, and the copper's own MOVEs to a copper pointer register. The resolved
        // address is drawn as a link after the details text; ptrNote labels it. Empty note plus
        // ptrResolved == UINT32_MAX means the row has nothing to add.
        std::string ptrRegName;            // "COP1LCH", "COPJMP2", ...
        std::string ptrNote;               // "jumps to", "list now", ... drawn before the link
        uint32_t ptrPc = 0;                // writing CPU instruction
        uint32_t ptrResolved = UINT32_MAX; // composed pointer, or a strobe's actual target
        bool ptrIsStrobe = false;          // COPJMP rather than COPxLC
    };
    std::vector<FormattedCopperRow> m_formattedLog;

    // Inbound scrubber position (set externally, UINT32_MAX = none)
    uint32_t m_scrubberLRef = UINT32_MAX;

    // Trace mode state
    int m_selectedTraceIdx = 0;
    bool m_traceScrollToSelected = false;
    int m_lastScrubberHighlightRowTrace = -1;

    static constexpr uint32_t kCCKPerLine = adm::core::constants::beam::CCKPerLine;

    void RebuildExecutionLog();
    void RebuildPointerWrites();
    void RebuildFormattedLog();
    void DrawMidFrameBadge();
    void DrawContent();
    void DrawLiveMode();
    void DrawTraceMode();

    // Render a Details cell. For MOVE instructions the register name is drawn as a clickable
    // hyperlink that navigates to the Custom Registers window; everything else is plain text.
    void DrawDetailsCell(adm::core::CopperInstruction::Type type, uint16_t word1, uint16_t word2,
                         const std::string& details, uint32_t addr, int traceIdx = -1);

    // Render an instruction address as a clickable hyperlink to the Memory window.
    void DrawCopperAddressLink(uint32_t addr);
};

} // namespace adm::ui
