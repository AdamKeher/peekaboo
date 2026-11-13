#include "BootBlockPreview.h"

#include <filesystem>

#include "Logger.h"

namespace adm::core {

BootBlockPreview::~BootBlockPreview() { Destroy(); }

bool BootBlockPreview::Start(const std::vector<uint8_t>& sector0,
                             const std::vector<uint8_t>& sector1)
{
    if (sector0.empty() || sector1.empty())
        return false;

    if (!m_emu)
        m_emu = std::make_unique<adm::AmigaEmulator>();
    // The main emulator's ROM folder, set again every start so a change since the last is followed
    if (!m_romDirectory.empty()) {
        m_emu->SetSystemDirectory(m_romDirectory);
    } else {
        try {
            m_emu->SetSystemDirectory(std::filesystem::canonical("./emulation").string());
        } catch (const std::exception& e) {
            Logger::Warn("Boot block preview: emulation dir not found: {}", e.what());
        }
    }
    if (!m_emu->IsCoreLoaded()) {
        // A *distinct* dylib copy gives this second emulator its own core globals.
        m_corePath = adm::AmigaEmulator::DuplicateCoreToTemp();
        if (m_corePath.empty()) {
            Logger::Error("Boot block preview: failed to load isolated emulator core.");
            Destroy();
            return false;
        }
        // LoadCore only null-checks the libretro entry points; the adm_* symbols are
        // dereferenced unconditionally per frame, so preflight the full required set.
        std::string coreError;
        if (!adm::AmigaEmulator::VerifyCore(m_corePath, coreError) ||
            !m_emu->LoadCore(m_corePath)) {
            Logger::Error("Boot block preview: failed to load isolated emulator core. {}",
                          coreError);
            Destroy();
            return false;
        }
        m_emu->SetMuted(true); // unmuted only while the preview tab is focused
    } else {
        // Reuse the existing isolated core, but reset its internal state first. PuAE
        // wedges after ~9 in-place reloads, so cycle retro_deinit()/retro_init() on the
        // same dylib (no fd/dylib churn) to give each preview a clean machine.
        m_emu->ReinitCore();
    }

    const auto& models = adm::GetAllAmigaModels();
    if (m_modelIdx >= 0 && m_modelIdx < static_cast<int>(models.size()))
        m_emu->SetModel(models[m_modelIdx].model);

    if (!m_emu->RunBootBlock(sector0, sector1)) {
        Logger::Error("Boot block preview: failed to boot the boot block.");
        return false;
    }

    m_booted = true;
    // Fast-forward through the Kickstart boot + boot-block load, then drop to real-time.
    m_warmup = true;
    m_emu->SetSpeedMultiplier(20.0);
    return true;
}

void BootBlockPreview::Stop()
{
    m_booted = false;
    m_warmup = false;
    if (m_emu)
        m_emu->SetMuted(true);
}

void BootBlockPreview::Destroy()
{
    m_booted = false;
    m_warmup = false;
    m_emu.reset(); // dtor releases the audio device + unloads the core

    // Remove the duplicated core file now that it is unloaded.
    if (!m_corePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_corePath, ec);
        m_corePath.clear();
    }
}

void BootBlockPreview::Tick(bool active, bool focused)
{
    if (!m_emu)
        return;

    // Mute during fast-forward warmup (avoids chipmunk audio); otherwise gate on focus.
    m_emu->SetMuted(m_warmup || !focused);

    if (!active || !m_booted)
        return;

    m_emu->Update();

    if (m_warmup) {
        // Exit turbo the moment drive activity is detected. The latch captures any LED
        // activation that occurred within the multi-frame Update() batch — IsDriveActive()
        // alone misses activity that started and finished inside a single 20x burst.
        // Machine init is pure ROM, so the first disk activity is the bootblock read;
        // dropping to 1x here lets the load and execution both run at normal speed.
        // Hard cap at ~12 emulated seconds as a fallback.
        if (m_emu->ConsumeDriveActiveLatch() || m_emu->GetTotalFrameCount() >= 600) {
            m_emu->SetSpeedMultiplier(1.0);
            m_warmup = false;
        }
    }
}

void BootBlockPreview::SetModelIndex(int idx)
{
    const auto& models = adm::GetAllAmigaModels();
    if (idx < 0 || idx >= static_cast<int>(models.size()))
        return;
    m_modelIdx = idx;
    if (m_emu)
        m_emu->SetModel(models[idx].model);
}

void BootBlockPreview::SendKey(bool down, adm::AmigaEmulator::RetroKey key)
{
    if (m_emu)
        m_emu->SendKeyEvent(down, key);
}

void BootBlockPreview::SetMouseState(int16_t dx, int16_t dy, bool left, bool right, bool middle)
{
    if (m_emu)
        m_emu->SetMouseState(dx, dy, left, right, middle);
}

unsigned int BootBlockPreview::TextureId() const { return m_emu ? m_emu->GetTextureID() : 0; }

float BootBlockPreview::AspectRatio() const { return m_emu ? m_emu->GetAspectRatio() : 1.0f; }

} // namespace adm::core
