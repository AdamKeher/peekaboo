#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "Subprocess.h"

namespace adm {
namespace core {

enum class ReadState { Idle, Reading, Done, Error, Cancelled };

enum class TrackReadStatus { Unloaded, Queued, Reading, Loaded, Verifying, Verified, Failed };

struct ReadConfig
{
    std::string drive = "A";
    std::string format = "amiga.amigados";
    // When set, every cylinder is read a second time after the first pass and the two reads are
    // compared byte-for-byte; a mismatch marks that track Failed instead of Verified. Roughly
    // doubles read time in exchange for catching a flaky drive/media that decoded cleanly but
    // inconsistently.
    bool verify = false;
};

struct ReadProgress
{
    int currentCylinder = 0;
    int totalCylinders = 80;
    float progressPercent = 0.0f;
    std::string statusMessage;
    int cylindersLoaded = 0;
    // Per-track status, indexed like GreaseweazelWriter's trackStatuses (cyl * 2 + head) — lets the
    // UI drive the same track-grid visual for a read as for a write.
    std::vector<TrackReadStatus> trackStatuses;
};

class GreaseweazelReader
{
public:
    static constexpr int kTotalCylinders = 80;
    static constexpr int kHeadsPerCylinder = 2;
    static constexpr int kTotalTracks = kTotalCylinders * kHeadsPerCylinder; // 160
    static constexpr int kSectorsPerTrack = 11;
    static constexpr int kBytesPerSector = 512;
    static constexpr int kBytesPerTrack = kSectorsPerTrack * kBytesPerSector; // 5632
    static constexpr int kTotalBlocks = kTotalTracks * kSectorsPerTrack;      // 1760
    static constexpr int kTotalBytes = kTotalBlocks * kBytesPerSector;        // 901120

    GreaseweazelReader() = default;
    ~GreaseweazelReader();

    GreaseweazelReader(const GreaseweazelReader&) = delete;
    GreaseweazelReader& operator=(const GreaseweazelReader&) = delete;

    /**
     * @brief Start initial read of boot (cyl 0) and root (cyl 40) cylinders.
     */
    void Start(const ReadConfig& config);

    /**
     * @brief Cancel any in-progress read.
     */
    void Cancel();

    /**
     * @brief Queue a cylinder for background reading.
     */
    void RequestCylinder(int cyl);

    /**
     * @brief Check if a track's data has been loaded.
     */
    bool IsTrackLoaded(int track) const;

    /**
     * @brief Check if a specific block's track has been loaded.
     */
    bool IsBlockLoaded(uint32_t block) const;

    /**
     * @brief Read sector data from the buffer. Returns zeros if track not loaded.
     */
    bool ReadSector(uint32_t sector, uint32_t sizeBlocks, uint8_t* buf);

    /**
     * @brief Atomically copy one block (kBytesPerSector) into dst, but only if its track is
     * already loaded. Returns true if the block was copied, false if not yet loaded or out of
     * range. Thread-safe — performs the loaded-check and copy under a single lock so the
     * background reader cannot concurrently overwrite the bytes being read.
     */
    bool CopyBlockIfLoaded(uint32_t block, uint8_t* dst) const;

    /**
     * @brief Snapshot the full 880KB buffer under the lock.
     *
     * Returns a copy rather than a reference on purpose: the reader thread writes whole tracks
     * into m_buffer as they arrive, so a caller holding a reference reads bytes that are being
     * rewritten underneath it. This is the same guarantee CopyBlockIfLoaded gives, for the whole
     * image. Tracks not yet read are zeros - check GetState() or the progress if that matters.
     */
    std::vector<uint8_t> CopyBuffer() const;

    ReadState GetState() const { return m_state.load(); }
    ReadProgress GetProgress() const;
    std::string GetOutput() const;

    /**
     * @brief Check if new tracks have been loaded since last call.
     * Resets the flag after reading.
     */
    bool HasNewData();

    /**
     * @brief Set callback invoked (from background thread) when new tracks arrive.
     */
    void SetOnTracksLoaded(std::function<void()> callback);

private:
    void RunReader();
    bool ReadCylinders(const std::vector<int>& cylinders);
    // Runs one `gw read` pass over the given cylinders and returns the resulting kTotalBytes ADF
    // image in outData (zero-padded if `gw` produced a shorter file). Used once per batch normally,
    // twice when ReadConfig::verify is set. phaseLabel ("Reading"/"Verifying") only affects the
    // logged command and the status message.
    bool RunGwReadPass(const std::vector<int>& cylinders, std::vector<uint8_t>& outData,
                       const char* phaseLabel);
    void ParseOutputLine(const std::string& line);

    ReadConfig m_config;
    std::vector<uint8_t> m_buffer;
    std::vector<bool> m_trackLoaded; // 160 entries
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<int> m_pendingCylinders;
    // Guarded by m_mutex. Keeps a cylinder from being enqueued more than once while its read is
    // pending — ReadSector re-requests unloaded cylinders on every adflib access.
    std::array<bool, kTotalCylinders> m_cylinderQueued{};
    std::atomic<ReadState> m_state{ReadState::Idle};
    std::atomic<bool> m_hasNewData{false};
    std::atomic<bool> m_shutdownRequested{false};
    std::thread m_thread;
    // The running `gw` subprocess. Run() is driven on the reader thread; Cancel() is called from
    // the main thread (and the destructor) to terminate it — Subprocess handles that safely.
    Subprocess m_proc;

    ReadProgress m_progress;
    std::string m_outputLog;
    std::string m_tempFilePath;
    std::function<void()> m_onTracksLoaded;

    void CleanupTempFile();
};

} // namespace core
} // namespace adm
