#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AmigaEmulator.h"

namespace adm::core {

/**
 * @brief The isolated second emulator behind the Boot Block Viewer's live-preview tab.
 *
 * Runs a boot block on its own machine, entirely separate from the main emulator, so previewing
 * one does not disturb whatever is loaded. Owns that machine, the private copy of the core it
 * loads, and the fast-forward warmup state.
 *
 * Deliberately knows nothing about the view. Whether the preview should be running at all, and
 * whether its tab is on screen and focused, are questions only the UI can answer - Application
 * answers them and calls Tick(). Everything below this line is machine mechanics.
 *
 * Extracted from Application.
 */
class BootBlockPreview
{
public:
    BootBlockPreview() = default;
    ~BootBlockPreview();

    BootBlockPreview(const BootBlockPreview&) = delete;
    BootBlockPreview& operator=(const BootBlockPreview&) = delete;

    /**
     * @brief Boot the given boot block on the isolated machine.
     *
     * Creates the machine on first call and reuses it afterwards. Returns false if the core
     * could not be loaded or the boot block would not boot; the caller tells the view, since
     * only the caller knows there is a view.
     */
    bool Start(const std::vector<uint8_t>& sector0, const std::vector<uint8_t>& sector1);

    /// The ROM folder the machine loads its Kickstart from: the main emulator's, so a ROM folder
    /// chosen in Preferences reaches the preview too. Empty = ./emulation. Read by Start().
    void SetRomDirectory(std::string directory) { m_romDirectory = std::move(directory); }

    /**
     * @brief Halt the preview but keep the machine alive for reuse.
     *
     * Destroying and recreating the emulator on every stop/start churns a CoreAudio device per
     * cycle and exhausts the macOS audio HAL after ~9 cycles - which also surfaces as failures
     * opening unrelated resources, e.g. TOSEC downloads. Start()'s reuse branch resets the kept
     * instance instead.
     */
    void Stop();

    /// Full teardown, including the duplicated core file. For app shutdown and for the
    /// initial-load-failure path; normal stop/start goes through Stop().
    void Destroy();

    /**
     * @brief Advance the preview one host frame.
     *
     * `active` is whether the preview tab is on screen, `focused` whether it has the input
     * focus. Audio is gated on focus (and muted outright during warmup, which would otherwise
     * play back chipmunked). No-op when nothing has been booted.
     */
    void Tick(bool active, bool focused);

    /// Machine model to boot under, as an index into adm::GetAllAmigaModels(). Applies to the
    /// running machine immediately and to the next Start().
    void SetModelIndex(int idx);

    /// True once a boot block has actually booted, i.e. there is a picture to show.
    bool IsBooted() const { return m_booted; }
    /// True while the isolated machine exists, booted or not.
    bool IsAlive() const { return m_emu != nullptr; }

    void SendKey(bool down, adm::AmigaEmulator::RetroKey key);
    void SetMouseState(int16_t dx, int16_t dy, bool left, bool right, bool middle);

    /// GL texture holding the preview's current frame, 0 when there is nothing to show.
    unsigned int TextureId() const;
    float AspectRatio() const;

private:
    std::unique_ptr<adm::AmigaEmulator> m_emu;
    bool m_booted = false;
    int m_modelIdx = 1; // default A500 (512 chip + 512 slow)
    std::string m_corePath;
    std::string m_romDirectory;
    /// While true the preview runs fast-forwarded (turbo) to skip the Kickstart boot +
    /// boot-block load, then drops back to real-time once the drive goes idle (or a frame cap
    /// is hit).
    bool m_warmup = false;
};

} // namespace adm::core
