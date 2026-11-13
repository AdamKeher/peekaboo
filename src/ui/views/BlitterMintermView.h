#pragma once

#include <cstdint>
#include <string>

namespace adm::ui {

class TextureManager;

// Interactive illustrator for the Amiga blitter's logic function (minterm / LF byte),
// modelled on the deadliners.net BLTCON cheat sheet by Frederic Condolo.
//
// The blitter combines up to three source channels A, B and C into destination D using an
// 8-bit logic function stored in the low byte of BLTCON0. Each of the 8 bits answers one row
// of the A/B/C truth table: bit n (n = A*4 + B*2 + C) is the value written to D for that
// combination of inputs. This window lets you toggle the truth table, pick common presets and
// channel/shift options, and watch a live 64x64 preview of how A, B and C combine into D.
class BlitterMintermView
{
public:
    explicit BlitterMintermView(TextureManager* textures) : m_textures(textures) {}
    ~BlitterMintermView();

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

    // Load a minterm from elsewhere (e.g. a selected blit). Re-derives channel usage.
    void SetMinterm(uint8_t minterm);

    // Load the full control state from a selected blit's BLTCON0/BLTCON1 and open the window.
    // Used by the explicit minterm hyperlinks — always applies regardless of the sync toggle.
    void LoadFromBlit(uint16_t bltcon0, uint16_t bltcon1);

    // Follow the currently-selected blit (scrubber / blitter row / scrubber object). Applies
    // only when the Sync toggle is on and the window is open; never forces the window open.
    // Re-selecting the same blit undoes whatever was edited here in the meantime.
    void SyncToBlit(uint16_t bltcon0, uint16_t bltcon1);

    // Assembled control words from the current state.
    uint16_t Bltcon0() const;
    uint16_t Bltcon1() const;

    // Build a readable sum-of-products expression for a minterm
    // (e.g. "A\xC2\xB7""B + !A\xC2\xB7""C"). Returns "0" / "1" for the constants.
    static std::string Expression(uint8_t minterm);

private:
    TextureManager* m_textures = nullptr;

    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;

    uint8_t m_minterm = 0xCA; // default: cookie-cut (A\xC2\xB7""B + !A\xC2\xB7""C)
    bool m_useA = true, m_useB = true, m_useC = true, m_useD = true;
    bool m_autoChannels = true;

    // When on (default), the window follows the selected blit across the app; when off it only
    // changes via the explicit minterm hyperlinks.
    bool m_sync = true;

    int m_ash = 0;                                     // shift A (BLTCON0 bits 15-12)
    int m_bsh = 0;                                     // shift B (BLTCON1 bits 15-12)
    bool m_efe = false, m_ife = false, m_desc = false; // BLTCON1 fill / direction
    bool m_line = false;                               // BLTCON1 bit 0 = line-draw mode

    // BLTCON1 bits this window does not model, kept verbatim so a blit's control word survives
    // the round trip through the editor. In line mode that includes bits 4-1, which carry
    // SUD/SUL/AUL/SING there rather than the fill flags they are in area mode.
    uint16_t m_con1Extra = 0;

    // Live 64x64 preview textures for the four channels (0 = not yet built).
    unsigned int m_texA = 0, m_texB = 0, m_texC = 0, m_texD = 0;
    bool m_visDirty = true;

    void DrawChannels();
    void DrawTruthTable();
    void DrawCanvases();
    void DrawSummary();
    void DrawPresets();
    void DrawBltcon();

    // Set USEA/USEB/USEC from whether the function actually depends on each input.
    void SyncAutoChannels();

    // Rebuild the four preview textures from the current minterm / channel / shift state.
    void RebuildTextures();
    void ReleaseTextures();
    void MarkDirty() { m_visDirty = true; }

    // Apply a BLTCON0/BLTCON1 word pair to the editable state.
    void ApplyBltcon(uint16_t bltcon0, uint16_t bltcon1);
};

} // namespace adm::ui
