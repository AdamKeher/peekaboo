#include "GreaseweazelReader.h"

#include <fmt/format.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

#include "Logger.h"

namespace adm {
namespace core {

GreaseweazelReader::~GreaseweazelReader()
{
    Cancel();
    m_shutdownRequested.store(true);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    CleanupTempFile();
}

void GreaseweazelReader::CleanupTempFile()
{
    if (!m_tempFilePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_tempFilePath, ec);
        m_tempFilePath.clear();
    }
}

void GreaseweazelReader::Start(const ReadConfig& config)
{
    // Wait for any previous thread to finish
    if (m_thread.joinable()) {
        m_shutdownRequested.store(true);
        m_cv.notify_all();
        m_thread.join();
    }

    CleanupTempFile();

    m_config = config;
    m_buffer.assign(kTotalBytes, 0);
    m_trackLoaded.assign(kTotalTracks, false);
    m_shutdownRequested.store(false);
    m_hasNewData.store(false);

    // Clear pending queue
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::queue<int> empty;
        m_pendingCylinders.swap(empty);
        m_cylinderQueued.fill(false);
        m_progress = {};
        m_progress.totalCylinders = kTotalCylinders;
        m_progress.trackStatuses.assign(kTotalTracks, TrackReadStatus::Unloaded);
        m_progress.statusMessage = "Starting floppy read...";
        m_outputLog.clear();

        // Queue all 80 cylinders for a full disk read
        for (int cyl = 0; cyl < kTotalCylinders; ++cyl) {
            m_pendingCylinders.push(cyl);
            m_cylinderQueued[cyl] = true;
        }
    }

    m_state.store(ReadState::Reading);
    m_thread = std::thread(&GreaseweazelReader::RunReader, this);
}

void GreaseweazelReader::Cancel()
{
    auto state = m_state.load();
    if (state == ReadState::Reading) {
        m_state.store(ReadState::Cancelled);
        // Terminate the running `gw` child (process group on POSIX, TerminateProcess on Windows);
        // the reader thread then breaks out of its output loop and ReadCylinders returns.
        m_proc.Cancel();
        m_cv.notify_all();
    }
}

void GreaseweazelReader::RequestCylinder(int cyl)
{
    if (cyl < 0 || cyl >= kTotalCylinders)
        return;

    // Check if both tracks for this cylinder are already loaded
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        int track0 = cyl * 2;
        int track1 = cyl * 2 + 1;
        if (m_trackLoaded[track0] && m_trackLoaded[track1])
            return;

        // MemoryDevice::ReadSector calls this for every unloaded block of every adflib read;
        // while a track is in flight the same cylinder would otherwise be enqueued thousands
        // of times and the queue would grow without bound.
        if (m_cylinderQueued[cyl])
            return;
        m_cylinderQueued[cyl] = true;
        m_pendingCylinders.push(cyl);
    }
    m_cv.notify_one();
}

bool GreaseweazelReader::IsTrackLoaded(int track) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (track < 0 || track >= kTotalTracks)
        return false;
    return m_trackLoaded[track];
}

bool GreaseweazelReader::IsBlockLoaded(uint32_t block) const
{
    if (block >= static_cast<uint32_t>(kTotalBlocks))
        return false;
    int track = static_cast<int>(block) / kSectorsPerTrack;
    return IsTrackLoaded(track);
}

bool GreaseweazelReader::ReadSector(uint32_t sector, uint32_t sizeBlocks, uint8_t* buf)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t offset = static_cast<size_t>(sector) * kBytesPerSector;
    size_t bytes = static_cast<size_t>(sizeBlocks) * kBytesPerSector;

    if (offset + bytes > m_buffer.size()) {
        return false;
    }

    // Copy data (zeros if not yet loaded)
    std::memcpy(buf, m_buffer.data() + offset, bytes);

    return true;
}

bool GreaseweazelReader::CopyBlockIfLoaded(uint32_t block, uint8_t* dst) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (block >= static_cast<uint32_t>(kTotalBlocks))
        return false;

    int track = static_cast<int>(block) / kSectorsPerTrack;
    if (track < 0 || track >= kTotalTracks || !m_trackLoaded[track])
        return false;

    size_t offset = static_cast<size_t>(block) * kBytesPerSector;
    if (offset + kBytesPerSector > m_buffer.size())
        return false;

    std::memcpy(dst, m_buffer.data() + offset, kBytesPerSector);
    return true;
}

std::vector<uint8_t> GreaseweazelReader::CopyBuffer() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer;
}

ReadProgress GreaseweazelReader::GetProgress() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

std::string GreaseweazelReader::GetOutput() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_outputLog;
}

bool GreaseweazelReader::HasNewData() { return m_hasNewData.exchange(false); }

void GreaseweazelReader::SetOnTracksLoaded(std::function<void()> callback)
{
    m_onTracksLoaded = std::move(callback);
}

void GreaseweazelReader::RunReader()
{
    while (!m_shutdownRequested.load()) {
        std::vector<int> batch;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_pendingCylinders.empty() || m_shutdownRequested.load() ||
                       m_state.load() == ReadState::Cancelled;
            });

            if (m_shutdownRequested.load() || m_state.load() == ReadState::Cancelled) {
                break;
            }

            // Collect unique cylinders from queue
            std::set<int> uniqueCyls;
            while (!m_pendingCylinders.empty()) {
                int cyl = m_pendingCylinders.front();
                m_pendingCylinders.pop();
                m_cylinderQueued[cyl] = false;
                // Only queue if not already loaded
                int t0 = cyl * 2;
                int t1 = cyl * 2 + 1;
                if (!m_trackLoaded[t0] || !m_trackLoaded[t1]) {
                    uniqueCyls.insert(cyl);
                }
            }
            batch.assign(uniqueCyls.begin(), uniqueCyls.end());
        }

        if (batch.empty())
            continue;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int cyl : batch) {
                for (int head = 0; head < kHeadsPerCylinder; ++head) {
                    int track = cyl * kHeadsPerCylinder + head;
                    if (track < static_cast<int>(m_progress.trackStatuses.size()))
                        m_progress.trackStatuses[track] = TrackReadStatus::Queued;
                }
            }
        }

        if (!ReadCylinders(batch)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int cyl : batch) {
                for (int head = 0; head < kHeadsPerCylinder; ++head) {
                    int track = cyl * kHeadsPerCylinder + head;
                    if (track < static_cast<int>(m_progress.trackStatuses.size()) &&
                        m_progress.trackStatuses[track] != TrackReadStatus::Loaded)
                        m_progress.trackStatuses[track] = TrackReadStatus::Failed;
                }
            }
            if (m_state.load() != ReadState::Cancelled) {
                m_state.store(ReadState::Error);
            }
            break;
        }

        // Check if all tracks are now loaded
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            bool allLoaded = true;
            for (int i = 0; i < kTotalTracks; ++i) {
                if (!m_trackLoaded[i]) {
                    allLoaded = false;
                    break;
                }
            }
            if (allLoaded && m_pendingCylinders.empty()) {
                int failedCount = 0;
                for (auto status : m_progress.trackStatuses) {
                    if (status == TrackReadStatus::Failed)
                        ++failedCount;
                }
                m_progress.statusMessage =
                    failedCount > 0
                        ? fmt::format("All tracks loaded ({} failed verification)", failedCount)
                        : "All tracks loaded";
                m_progress.progressPercent = 1.0f;
                m_state.store(ReadState::Done);
                break;
            }
        }
    }

    CleanupTempFile();
}

bool GreaseweazelReader::RunGwReadPass(const std::vector<int>& cylinders,
                                       std::vector<uint8_t>& outData, const char* phaseLabel)
{
    // Build cylinder spec with range compression: [0,1,2,3,5,7,8,9] → "0-3,5,7-9"
    std::string cylSpec;
    size_t i = 0;
    while (i < cylinders.size()) {
        if (!cylSpec.empty())
            cylSpec += ",";
        size_t rangeStart = i;
        while (i + 1 < cylinders.size() && cylinders[i + 1] == cylinders[i] + 1)
            ++i;
        if (i == rangeStart) {
            cylSpec += fmt::format("{}", cylinders[rangeStart]);
        } else {
            cylSpec += fmt::format("{}-{}", cylinders[rangeStart], cylinders[i]);
        }
        ++i;
    }

    // Create temp file path for the output ADF
    auto tempDir = std::filesystem::temp_directory_path();
    auto tempPath = tempDir / "adm_gw_read.adf";
    m_tempFilePath = tempPath.string();

    // Build gw read argument vector. We exec `gw` directly (no shell) so there is a real PID
    // to kill on Cancel and no shell-injection surface from the temp path / config strings.
    std::string tracksArg = fmt::format("--tracks=c={}", cylSpec);
    std::vector<std::string> args = {"gw",       "read",          "--drive", m_config.drive,
                                     "--format", m_config.format, tracksArg, m_tempFilePath};

    Logger::Info("GreaseweazelReader: Executing ({}): gw read --drive {} --format {} {} {}",
                 phaseLabel, m_config.drive, m_config.format, tracksArg, m_tempFilePath);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputLog += fmt::format("$ gw read --drive {} --format {} {} {}\n", m_config.drive,
                                   m_config.format, tracksArg, m_tempFilePath);
        m_progress.statusMessage = fmt::format("{} cylinder(s) {}...", phaseLabel, cylSpec);
    }

    // Spawn gw directly (no shell) and stream its combined stdout+stderr through ParseOutputLine.
    // Cancel() and the destructor call m_proc.Cancel() to terminate the child, which ends Run().
    int exitCode = m_proc.Run(args, [this](const std::string& line) { ParseOutputLine(line); });

    if (m_state.load() == ReadState::Cancelled || m_shutdownRequested.load()) {
        return false;
    }

    if (exitCode < 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = "Error: Failed to launch gw command";
        m_outputLog += "Error: Failed to launch gw command\n";
        return false;
    }

    if (exitCode != 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = fmt::format("Error: gw exited with code {}", exitCode);
        m_outputLog += fmt::format("\ngw read exited with code {}\n", exitCode);
        return false;
    }

    // Read the output ADF. gw produces a full 880KB ADF even with --tracks, with zeros for
    // unread tracks; pad with zeros too if it came back short so callers can index it uniformly.
    std::ifstream adfFile(m_tempFilePath, std::ios::binary | std::ios::ate);
    if (!adfFile) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = "Error: Failed to read output ADF";
        m_outputLog += "Error: Failed to open output ADF file\n";
        return false;
    }

    auto fileSize = adfFile.tellg();
    adfFile.seekg(0, std::ios::beg);

    outData.assign(kTotalBytes, 0);
    std::streamsize toRead = static_cast<std::streamsize>(fileSize) < kTotalBytes
                                 ? static_cast<std::streamsize>(fileSize)
                                 : static_cast<std::streamsize>(kTotalBytes);
    if (toRead > 0) {
        adfFile.read(reinterpret_cast<char*>(outData.data()), toRead);
    }
    adfFile.close();

    CleanupTempFile();
    return true;
}

bool GreaseweazelReader::ReadCylinders(const std::vector<int>& cylinders)
{
    if (cylinders.empty())
        return true;

    std::vector<uint8_t> pass1;
    if (!RunGwReadPass(cylinders, pass1, "Reading"))
        return false;

    // Commit the first pass into the disk buffer straight away - this is the data that ends up in
    // the mounted document even when verify is on; the second pass below only checks it.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int cyl : cylinders) {
            for (int head = 0; head < kHeadsPerCylinder; ++head) {
                int track = cyl * kHeadsPerCylinder + head;
                size_t trackOffset =
                    static_cast<size_t>(track) * kSectorsPerTrack * kBytesPerSector;
                size_t trackBytes = static_cast<size_t>(kSectorsPerTrack) * kBytesPerSector;

                std::memcpy(m_buffer.data() + trackOffset, pass1.data() + trackOffset, trackBytes);
                m_trackLoaded[track] = true;
                if (track < static_cast<int>(m_progress.trackStatuses.size()))
                    m_progress.trackStatuses[track] = TrackReadStatus::Loaded;
            }
            m_progress.cylindersLoaded++;
        }
        m_progress.progressPercent =
            static_cast<float>(m_progress.cylindersLoaded) / static_cast<float>(kTotalCylinders);
    }

    if (m_config.verify) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int cyl : cylinders) {
                for (int head = 0; head < kHeadsPerCylinder; ++head) {
                    int track = cyl * kHeadsPerCylinder + head;
                    if (track < static_cast<int>(m_progress.trackStatuses.size()))
                        m_progress.trackStatuses[track] = TrackReadStatus::Verifying;
                }
            }
        }

        std::vector<uint8_t> pass2;
        bool verifyRan = RunGwReadPass(cylinders, pass2, "Verifying");

        if (m_state.load() == ReadState::Cancelled || m_shutdownRequested.load()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!verifyRan) {
            // The data from pass 1 is still good; we just couldn't confirm it. Leave it Loaded
            // rather than Failed, and say so once in the log instead of per-track.
            m_outputLog += "Warning: verify pass failed to run; keeping the initial read "
                           "unverified.\n";
            for (int cyl : cylinders) {
                for (int head = 0; head < kHeadsPerCylinder; ++head) {
                    int track = cyl * kHeadsPerCylinder + head;
                    if (track < static_cast<int>(m_progress.trackStatuses.size()))
                        m_progress.trackStatuses[track] = TrackReadStatus::Loaded;
                }
            }
        } else {
            for (int cyl : cylinders) {
                for (int head = 0; head < kHeadsPerCylinder; ++head) {
                    int track = cyl * kHeadsPerCylinder + head;
                    size_t trackOffset =
                        static_cast<size_t>(track) * kSectorsPerTrack * kBytesPerSector;
                    size_t trackBytes = static_cast<size_t>(kSectorsPerTrack) * kBytesPerSector;
                    bool match = std::memcmp(pass1.data() + trackOffset, pass2.data() + trackOffset,
                                             trackBytes) == 0;
                    if (track < static_cast<int>(m_progress.trackStatuses.size())) {
                        m_progress.trackStatuses[track] =
                            match ? TrackReadStatus::Verified : TrackReadStatus::Failed;
                    }
                    if (!match) {
                        m_outputLog += fmt::format("Verify mismatch on cylinder {} head {} (track "
                                                   "{}) - re-read differs from the first read.\n",
                                                   cyl, head, track);
                    }
                }
            }
        }
    }

    // Notify listeners outside the lock: the callback runs on this background thread and may call
    // back into the reader (e.g. GetProgress()), which would self-deadlock on the non-recursive
    // mutex if invoked while still holding it.
    m_hasNewData.store(true);
    if (m_onTracksLoaded) {
        m_onTracksLoaded();
    }

    Logger::Info("GreaseweazelReader: Loaded {} cylinder(s), total {}/{}", cylinders.size(),
                 m_progress.cylindersLoaded, kTotalCylinders);

    return true;
}

void GreaseweazelReader::ParseOutputLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outputLog += line + "\n";
    m_progress.statusMessage = line;

    // Try to parse track progress: "T<cyl>.<head>". Compiled once — regex construction is
    // expensive and this runs for every line of gw output.
    static const std::regex trackRegex(R"(T(\d+)\.(\d+))");
    std::smatch match;
    if (std::regex_search(line, match, trackRegex)) {
        int cyl = std::stoi(match[1].str());
        int head = std::stoi(match[2].str());
        m_progress.currentCylinder = cyl;
        m_progress.cylindersLoaded = cyl + 1;
        m_progress.progressPercent =
            static_cast<float>(cyl + 1) / static_cast<float>(kTotalCylinders);

        int track = cyl * kHeadsPerCylinder + head;
        if (track >= 0 && track < static_cast<int>(m_progress.trackStatuses.size()) &&
            m_progress.trackStatuses[track] != TrackReadStatus::Loaded &&
            m_progress.trackStatuses[track] != TrackReadStatus::Verifying) {
            m_progress.trackStatuses[track] = TrackReadStatus::Reading;
        }
    }
}

} // namespace core
} // namespace adm
