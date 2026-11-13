#pragma once

#include <imgui.h>
#include <cstdint>
#include <functional>
#include <vector>

#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"
#include "../../core/BitplaneDecoder.h"
#include "../../core/BitplaneLayout.h"
#include "../../core/UserPreferences.h"
#include "AddressActions.h"

typedef unsigned int GLuint;

namespace adm::core {
struct BlitRecord;
}

namespace adm::ui {

class EmulatorView;

// One detected bitplane configuration (a contiguous scanline range with consistent
// BPLxPT pointers, BPLCON0, and colour palette — derived from BplptEvents + copper trace).
struct BitplaneInstance
{
    uint16_t vpos_start = 0;
    uint16_t vpos_end = 0;
    int bplCount = 0; // BPU from bplcon0 (AGA extension bit included)
    // Horizontal resolution from BPLCON0 HIRES/SHRES. Stored as the full three-way resolution
    // rather than an isHires bool: the bool read bit 15 only, so super-hires splits decoded at
    // half width and were labelled LORES.
    core::BitplaneResolution resolution = core::BitplaneResolution::Lores;
    bool isHAM = false;
    bool isDblPF = false;
    uint32_t ptrs[8] = {};
    uint16_t bplcon0 = 0;
    uint16_t ddfstrt = 0;
    uint16_t ddfstop = 0;
    bool paletteValid = false;
    ImU32 palette[32] = {}; // COLOR00–31 snapshot at vpos_start
    GLuint texId = 0;
    int texW = 0;
    int texH = 0;
};

class BitplanesView
{
public:
    explicit BitplanesView(EmulatorView* emulatorView);
    ~BitplanesView();

    void Draw();
    void Clear();

    void SetOnJumpToMemory(std::function<void(uint32_t)> cb);
    void SetUserPreferences(core::UserPreferences* preferences);

    // Shared address hyperlink context (left-click jump + right-click actions menu). When set and
    // valid, bitplane pointer addresses render as the app-wide clickable links; otherwise they fall
    // back to the plain SetOnJumpToMemory affordance.
    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    // Read-only snapshot of the most recently decoded geometry, for overlay mapping.
    // Decodes geometry on demand (cheap, texture-free) so callers get a valid layout even
    // when the Bitplanes window isn't open. Non-const because it refreshes cached state.
    core::BitplaneLayout GetBitplaneLayout();

    // The blit whose destination should be highlighted on the matching plane + composite tiles
    // this frame (nullptr = none). Owned by the caller; valid only for the current Draw().
    void SetActiveBlit(const core::BlitRecord* rec) { m_activeBlit = rec; }

private:
    static constexpr int kSourceAuto = 0;
    static constexpr int kSourceFrameTrace = 1;
    static constexpr int kSourceManual = 2;
    static constexpr int kSourceFrameBoundary = 3;

    EmulatorView* m_emulatorView;
    core::UserPreferences* m_userPreferences = nullptr;
    const core::BlitRecord* m_activeBlit = nullptr;

    // ---- UI / user controls ----
    int m_sourceMode = kSourceAuto;
    bool m_autoDetect = true;        // auto-detect BPL count & dims from registers
    int m_manualBplCount = 3;        // manual BPL count override (1–8)
    int m_manualWidth = 320;         // manual screen width in pixels
    int m_manualHeight = 256;        // manual screen height in pixels
    bool m_useManualModulo1 = false; // override BPL1MOD
    int m_manualModulo1 = 0;         // BPL1MOD override value
    bool m_useManualModulo2 = false; // override BPL2MOD
    int m_manualModulo2 = 0;         // BPL2MOD override value
    bool m_interleavedAuto = true;   // auto-detect interleaved layout
    bool m_interleavedForce = false; // manual interleaved override
    bool m_useRegisterPointers = true;
    uint32_t m_manualPointers[core::BitplaneLayout::kMaxPlanes] = {};
    bool m_useManualStride1 = false;
    bool m_useManualStride2 = false;
    int m_manualStride1 = 40;
    int m_manualStride2 = 40;
    bool m_overrideWidth = false;  // always-accessible width override
    bool m_overrideHeight = false; // always-accessible height override
    bool m_applyPalette = false;   // use Amiga colours (else B/W)
    bool m_showComposite = true;   // show composite tile
    float m_zoom = 1.0f;           // global preview scale
    bool m_lockAspect = false;     // lock 1:1 pixel ratio (else tall Amiga pixels)
    bool m_fitToWindow = true;
    bool m_showInspector = true;

    // ---- trace mode ----
    bool m_traceMode = false;
    int m_selectedTraceRow = -1;
    int m_autoTraceRegion = -1;
    bool m_autoUsingTrace = false;
    std::vector<BitplaneInstance> m_traceInstances;
    std::vector<adm::AmigaEmulator::CopperExecEvent>
        m_copperEventBuf; // heap — avoids ~320 KB stack
    std::vector<adm::AmigaEmulator::BplptEvent> m_bplptEventBuf;
    std::function<void(uint32_t)> m_onJumpToMemory;
    const AddressActionContext* m_addrCtx = nullptr; // shared hyperlink + actions context

    // ---- decode gate ----
    // Re-decode only when the emulator advanced a frame (data changed) or the user changed a
    // control that affects the decode/textures. Paused + unchanged controls => no work.
    uint64_t m_lastDecodeEmuFrame = UINT64_MAX;
    uint64_t m_lastControlSig = 0;
    uint64_t m_lastGeomFrame = UINT64_MAX; // gate for the geometry-only decode
    uint64_t m_lastGeomControlSig = 0;
    uint64_t ControlSignature() const; // hash of all decode-affecting controls

    // ---- cached state from last decode ----
    core::Custom m_regs{};
    bool m_regsValid = false;

    // Resolved bitplane info
    static constexpr int kMaxPlanes = 8;
    int m_bplCount = 0;
    int m_screenWidth = 320;
    int m_screenHeight = 256;
    int m_modulo1 = 0;
    int m_modulo2 = 0;
    bool m_interleaved = false;
    uint32_t m_bplBase[kMaxPlanes] = {};
    core::BitplaneDecodeResult m_decodeResult;

    // ---- per-plane OpenGL textures ----
    GLuint m_planeTex[kMaxPlanes] = {};
    int m_planeTexW[kMaxPlanes] = {};
    int m_planeTexH[kMaxPlanes] = {};

    // ---- composite texture ----
    GLuint m_compositeTex = 0;
    int m_compositeTexW = 0;
    int m_compositeTexH = 0;
    std::vector<uint8_t> m_compositeColorIdx; // palette index per pixel
    bool m_previewPaletteOverride = false;
    uint16_t m_previewPalette[32] = {};

    // Chip RAM fast-path (set once per frame in EnsureDecoded)
    const uint8_t* m_chipRamHostPtr = nullptr;
    uint32_t m_chipRamAmigaBase = 0;
    uint32_t m_chipRamSize = 0;

    // Per-plane decoded 1bpp data (y*width + x = 0 or 1), supplied by BitplaneDecoder.
    std::vector<uint8_t> m_decodedBits[kMaxPlanes];

    // Persistent RGBA pixel buffer — shared, resized only when dimensions change
    std::vector<uint8_t> m_pixelBuf;

    // ---- internal helpers ----

    // Ensure decode/texture build happens once per Dear ImGui frame
    void EnsureDecoded();
    void EnsureGeometryDecoded(); // texture-free: fetch registers + ResolveGeometry, frame-gated
    void ResolveGeometry();       // registers → bplBase/screen/modulo/interleave (no textures)
    core::BitplaneDecodeOptions DecodeOptions() const;
    void ApplyDecodedLayout(const core::BitplaneLayout& layout);
    void DecodeBitplanes(const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);
    void ApplyDecodeResult(const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);
    void RebuildTraceInstances(const std::vector<adm::AmigaEmulator::MemoryRegion>& regions,
                               bool buildTextures);
    bool DecodeAutoTraceRegion(const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);
    bool IsUsableTraceRegion(int index) const;
    int BestTraceRegion() const;
    int AdjacentTraceRegion(int direction) const;
    uint16_t PreviewColor(int index) const;

    // Build a greyscale (or palette-mapped) RGBA texture for one bitplane
    void BuildPlaneTexture(int planeIdx, int width, int height,
                           const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);

    // Build an indexed composite RGBA texture (palette or 1-bit white)
    void BuildCompositeTexture(int bplCount, int width, int height,
                               const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);

    // Safe read of one byte from chip RAM (big-endian Amiga memory)
    bool ReadChipByte(uint32_t addr, uint8_t& out,
                      const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const;

    // Upload or replace an OpenGL RGBA texture
    static GLuint UploadRGBATexture(GLuint existing, int w, int h, const uint8_t* rgba,
                                    int prevW = 0, int prevH = 0);

    // Render a bitplane pointer address as the shared clickable hyperlink (left-click jumps to the
    // Memory window, right-click opens the address-actions menu). Falls back to plain text +
    // SetOnJumpToMemory when no address context is set. addr==0 renders a disabled "---".
    // idSuffix keeps the right-click popup id unique across the many links drawn per frame.
    void DrawPtrLink(const char* idSuffix, uint32_t addr);

    // Draw left-panel controls; returns true if any setting changed
    void DrawControls();

    void DrawToolbar();
    void DrawStatusBar();
    core::UserPreferences::Preferences::BitplanePrefs CurrentPreferences() const;
    void ApplyPreferences(const core::UserPreferences::Preferences::BitplanePrefs& preferences);

    // Draw the tiled bitplane grid
    void DrawGrid();

    // Draw the vertical palette swatch strip
    void DrawPalette();

    // Decode a single pixel row from a given plane at chip address rowAddr
    // Reads bytesPerRow bytes and writes 1-bit pixel data into pixelRow[0..width-1]
    void DecodeRow(uint32_t rowAddr, int bytesPerRow, uint8_t* pixelRow,
                   const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const;

    // ---- trace mode helpers ----

    // Build BitplaneInstance list from copper events (primary split detector) + BplptEvents
    // (pointer source). Splits on BPLCON0 mode changes and BPL1PTL writes.
    void BuildTraceInstances(const adm::AmigaEmulator::BplptEvent* bplEvents, int bplEvtCount,
                             const adm::AmigaEmulator::CopperExecEvent* copperEvents,
                             int copperCount);

    // Replay copper MOVE events to snapshot COLOR00–31 at each instance's vpos_start
    void AssignInstancePalettesFromCopper(const core::Custom& regs,
                                          const adm::AmigaEmulator::CopperExecEvent* events,
                                          int count);

    // Build a composite RGBA preview texture for one trace instance
    void BuildInstanceTexture(int instIdx,
                              const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);

    // Draw the trace-mode table (one row per instance)
    void DrawTraceTable();

    // Draw expanded detail for the selected trace row
    void DrawTraceExpandedRow(int instIdx);
};

} // namespace adm::ui
