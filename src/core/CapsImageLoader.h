#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief Runtime wrapper around the SPS/CAPS decoder plugin (capsimg).
 *
 * IPF reading has no open-source implementation; like every Amiga emulator we
 * delegate to the redistributable capsimg plugin, loaded at runtime via
 * dlopen/LoadLibrary (the same mechanism used for the PuAE core). When the
 * plugin is absent, Available() returns false and Decode() yields nullopt so
 * the caller can show a friendly "place capsimg here" message instead of
 * crashing.
 *
 * Decode() returns the raw MFM cell data per track; turning that into sectors
 * (and an ADF) is the job of AmigaMfmDecoder / IpfConverter.
 */
class CapsImageLoader
{
public:
    struct RawTrack
    {
        uint32_t cylinder = 0;
        uint32_t head = 0;
        std::vector<uint8_t> mfm; // MFM cell bytes, 8 cells per byte, MSB first
        uint32_t lengthBits = 0;  // physical track length in bits
        uint32_t weakCount = 0;   // number of weak/fuzzy data areas
        uint32_t trackType = 0;   // CAPS density classification (ctit*)
    };

    struct DecodedImage
    {
        std::vector<RawTrack> tracks;
        uint32_t minCylinder = 0;
        uint32_t maxCylinder = 0;
        uint32_t minHead = 0;
        uint32_t maxHead = 0;
    };

    static CapsImageLoader& Instance();

    /// True if the plugin loaded and every required symbol resolved.
    bool Available();

    /// Reason the plugin is unavailable (path tried / missing symbol). Empty when ok.
    const std::string& LastError() const { return m_lastError; }

    /// Decode an in-memory IPF image to per-track MFM. nullopt on failure.
    std::optional<DecodedImage> Decode(const std::vector<uint8_t>& ipfBytes);

    ~CapsImageLoader();

    CapsImageLoader(const CapsImageLoader&) = delete;
    CapsImageLoader& operator=(const CapsImageLoader&) = delete;

private:
    CapsImageLoader() = default;
    bool EnsureLoaded();

    void* m_handle = nullptr;
    bool m_triedLoad = false;
    bool m_initialised = false;
    std::string m_lastError;

    // Resolved capsimg entry points (see CapsLib.h for signatures).
    void* m_capsInit = nullptr;
    void* m_capsExit = nullptr;
    void* m_capsAddImage = nullptr;
    void* m_capsRemImage = nullptr;
    void* m_capsLockImageMemory = nullptr;
    void* m_capsUnlockImage = nullptr;
    void* m_capsGetImageInfo = nullptr;
    void* m_capsLockTrack = nullptr;
    void* m_capsUnlockTrack = nullptr;
};

} // namespace adm::core
