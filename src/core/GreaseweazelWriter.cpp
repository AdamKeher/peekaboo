#include "GreaseweazelWriter.h"

#include <fmt/format.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>

#include "Logger.h"

namespace adm {
namespace core {

GreaseweazelWriter::~GreaseweazelWriter()
{
    if (m_thread.joinable()) {
        Cancel();
        m_thread.join();
    }
    CleanupTempFile();
}

void GreaseweazelWriter::CleanupTempFile()
{
    if (!m_tempFilePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_tempFilePath, ec);
        m_tempFilePath.clear();
    }
}

void GreaseweazelWriter::Start(const WriteConfig& config)
{
    // Wait for any previous thread to finish
    if (m_thread.joinable()) {
        m_thread.join();
    }

    CleanupTempFile();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = {};
        m_progress.totalTracks = 160;
        m_progress.trackStatuses.assign(160, TrackStatus::Pending);
        m_outputLog.clear();
        m_outputHasError = false;
        m_progress.statusMessage = "Starting write...";
    }
    m_state.store(WriteState::Starting);

    m_thread = std::thread(&GreaseweazelWriter::RunWrite, this, config);
}

void GreaseweazelWriter::Cancel()
{
    auto state = m_state.load();
    if (state == WriteState::Writing || state == WriteState::Verifying ||
        state == WriteState::Starting) {
        m_state.store(WriteState::Cancelled);
        // Terminate the running gw child (process group on POSIX, TerminateProcess on Windows).
        // The worker thread then breaks out of its read loop and reaps the child.
        m_proc.Cancel();
    }
}

WriteProgress GreaseweazelWriter::GetProgress() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

std::string GreaseweazelWriter::GetOutput() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_outputLog;
}

std::string GreaseweazelWriter::BuildTrackRanges(const std::vector<bool>& zeroFilledBlocks,
                                                 int sectorsPerTrack) const
{
    // An Amiga DD disk has 80 cylinders x 2 heads = 160 tracks
    // Each track has sectorsPerTrack sectors (typically 11 for AmigaDOS)
    // Work at cylinder level: a cylinder needs writing if either head has data
    constexpr int totalCylinders = 80;

    std::vector<bool> cylHasData(totalCylinders, false);

    for (int cyl = 0; cyl < totalCylinders; ++cyl) {
        for (int head = 0; head < 2; ++head) {
            int track = cyl * 2 + head;
            int startBlock = track * sectorsPerTrack;
            for (int s = 0; s < sectorsPerTrack; ++s) {
                int block = startBlock + s;
                if (block < static_cast<int>(zeroFilledBlocks.size()) && !zeroFilledBlocks[block]) {
                    cylHasData[cyl] = true;
                    break;
                }
            }
            if (cylHasData[cyl])
                break;
        }
    }

    // Build TSPEC: comma-separated ranges within a single c= spec
    // e.g. "c=0-45,58-79" (commas separate ranges, colons separate param types)
    std::string cylSet;
    int rangeStart = -1;

    for (int c = 0; c < totalCylinders; ++c) {
        if (cylHasData[c]) {
            if (rangeStart < 0)
                rangeStart = c;
        } else {
            if (rangeStart >= 0) {
                if (!cylSet.empty())
                    cylSet += ",";
                if (rangeStart == c - 1) {
                    cylSet += fmt::format("{}", rangeStart);
                } else {
                    cylSet += fmt::format("{}-{}", rangeStart, c - 1);
                }
                rangeStart = -1;
            }
        }
    }
    // Close final range
    if (rangeStart >= 0) {
        if (!cylSet.empty())
            cylSet += ",";
        if (rangeStart == totalCylinders - 1) {
            cylSet += fmt::format("{}", rangeStart);
        } else {
            cylSet += fmt::format("{}-{}", rangeStart, totalCylinders - 1);
        }
    }

    return cylSet.empty() ? std::string() : fmt::format("c={}", cylSet);
}

void GreaseweazelWriter::ParseOutputLine(const std::string& line)
{
    // Greaseweazel outputs track progress like:
    // "Writing Track 0.0..." or "T0.0" or "0.0: blah"
    // We look for patterns like T<cyl>.<head> or "Track <cyl>.<head>"
    // Also look for verify output

    std::lock_guard<std::mutex> lock(m_mutex);

    m_outputLog += line + "\n";
    m_progress.statusMessage = line;

    // Detect error messages in gw output (gw may exit 0 even on failure)
    if (line.find("Command Failed") != std::string::npos ||
        line.find("command failed") != std::string::npos ||
        line.find("Error:") != std::string::npos || line.find("error:") != std::string::npos ||
        line.find("No Index") != std::string::npos ||
        line.find("FluxStatus") != std::string::npos) {
        m_outputHasError = true;
    }

    // Try to parse track progress: "T<cyl>.<head>" or similar patterns
    std::regex trackRegex(R"(T(\d+)\.(\d+))");
    std::smatch match;
    if (std::regex_search(line, match, trackRegex)) {
        int cyl = std::stoi(match[1].str());
        int head = std::stoi(match[2].str());
        int trackIndex = cyl * 2 + head;

        if (trackIndex >= 0 && trackIndex < static_cast<int>(m_progress.trackStatuses.size())) {
            m_progress.currentTrack = trackIndex;

            auto state = m_state.load();
            if (state == WriteState::Verifying) {
                // Mark any previously-verifying track as verified (handles gaps from fast write)
                for (auto& ts : m_progress.trackStatuses) {
                    if (ts == TrackStatus::Verifying)
                        ts = TrackStatus::Verified;
                }
                m_progress.trackStatuses[trackIndex] = TrackStatus::Verifying;
            } else {
                // Mark any previously-writing track as written (handles gaps from fast write)
                for (auto& ts : m_progress.trackStatuses) {
                    if (ts == TrackStatus::Writing)
                        ts = TrackStatus::Written;
                }
                m_progress.trackStatuses[trackIndex] = TrackStatus::Writing;
            }

            m_progress.progressPercent =
                static_cast<float>(trackIndex + 1) / static_cast<float>(m_progress.totalTracks);
        }
    }

    // Detect verify phase start
    if (line.find("Verif") != std::string::npos || line.find("verif") != std::string::npos) {
        // Don't change state here - let RunWrite handle it based on context
    }
}

int GreaseweazelWriter::RunGwProcess(const std::vector<std::string>& args,
                                     const std::function<void(const std::string&)>& onLine)
{
    return m_proc.Run(args, onLine);
}

void GreaseweazelWriter::RunInfo()
{
    auto state = m_state.load();
    if (state != WriteState::Idle && state != WriteState::Done && state != WriteState::Error &&
        state != WriteState::Cancelled) {
        Logger::Warn("Greaseweazle is busy (State: {})", static_cast<int>(state));
        return;
    }

    m_outputLog.clear();
    m_outputHasError = false;
    m_state = WriteState::Starting;
    m_progress = WriteProgress{};
    m_progress.statusMessage = "Getting info...";

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_thread = std::thread([this]() {
        Logger::Info("Executing: gw info");
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLog += "$ gw info\n";
        }

        m_state = WriteState::Writing;

        int result = RunGwProcess({"gw", "info"}, [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLog += line + "\n";
        });

        m_state = (result == 0) ? WriteState::Done : WriteState::Error;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = (result == 0) ? "Info complete" : "Command failed";
    });
}

void GreaseweazelWriter::RunBandwidth()
{
    auto state = m_state.load();
    if (state != WriteState::Idle && state != WriteState::Done && state != WriteState::Error &&
        state != WriteState::Cancelled) {
        Logger::Warn("Greaseweazle is busy (State: {})", static_cast<int>(state));
        return;
    }

    m_outputLog.clear();
    m_outputHasError = false;
    m_state = WriteState::Starting;
    m_progress = WriteProgress{};
    m_progress.statusMessage = "Measuring bandwidth...";

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_thread = std::thread([this]() {
        Logger::Info("Executing: gw bandwidth");
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLog += "$ gw bandwidth\n";
        }

        m_state = WriteState::Writing;

        int result = RunGwProcess({"gw", "bandwidth"}, [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLog += line + "\n";
        });

        m_state = (result == 0) ? WriteState::Done : WriteState::Error;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = (result == 0) ? "Bandwidth check complete" : "Command failed";
    });
}

void GreaseweazelWriter::RunRPM(const std::string& drive)
{
    auto state = m_state.load();
    if (state != WriteState::Idle && state != WriteState::Done && state != WriteState::Error &&
        state != WriteState::Cancelled) {
        Logger::Warn("Greaseweazle is busy (State: {})", static_cast<int>(state));
        return;
    }

    m_outputLog.clear();
    m_outputHasError = false;
    m_state = WriteState::Starting;
    m_progress = WriteProgress{};
    m_progress.statusMessage = "Measuring RPM...";

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_thread = std::thread([this, drive]() {
        Logger::Info("Executing: gw rpm --drive {}", drive);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLog += "$ gw rpm --drive " + drive + "\n";
        }

        m_state = WriteState::Writing;

        int result = RunGwProcess({"gw", "rpm", "--drive", drive}, [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_outputLog += line + "\n";
        });

        m_state = (result == 0) ? WriteState::Done : WriteState::Error;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = (result == 0) ? "RPM check complete" : "Command failed";
    });
}

void GreaseweazelWriter::RunWrite(WriteConfig config)
{
    // If raw ADF data is provided (ZIP/DMS source), write to a temp file
    std::string adfPath = config.adfPath;
    if (!config.adfData.empty()) {
        auto tempDir = std::filesystem::temp_directory_path();
        auto tempPath = tempDir / "adm_gw_write.adf";
        std::ofstream tempFile(tempPath, std::ios::binary);
        if (!tempFile) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.statusMessage = "Error: Failed to create temp ADF file";
            m_outputLog += "Error: Failed to create temp ADF file\n";
            m_state.store(WriteState::Error);
            return;
        }
        tempFile.write(reinterpret_cast<const char*>(config.adfData.data()),
                       static_cast<std::streamsize>(config.adfData.size()));
        tempFile.close();
        adfPath = tempPath.string();
        m_tempFilePath = adfPath;
        Logger::Info("GreaseweazelWriter: Wrote temp ADF ({} bytes) to {}", config.adfData.size(),
                     adfPath);
    }

    // Build argv (no shell — gw is exec'd directly by RunGwProcess on POSIX)
    std::vector<std::string> args = {"gw",         "write",    "--drive",
                                     config.drive, "--format", config.format};

    if (!config.verify)
        args.push_back("--no-verify");
    if (config.preErase)
        args.push_back("--pre-erase");
    if (config.eraseEmpty)
        args.push_back("--erase-empty");

    // Fast write: compute non-empty cylinder ranges
    if (config.fastWrite && !config.zeroFilledBlocks.empty()) {
        // AmigaDOS DD = 11 sectors per track
        int sectorsPerTrack = 11;
        std::string trackRanges = BuildTrackRanges(config.zeroFilledBlocks, sectorsPerTrack);

        if (!trackRanges.empty()) {
            args.push_back("--tracks=" + trackRanges);

            // Mark skipped tracks in progress (cylinder-level: skip only if both heads empty)
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int cyl = 0; cyl < 80; ++cyl) {
                bool cylHasData = false;
                for (int head = 0; head < 2 && !cylHasData; ++head) {
                    int track = cyl * 2 + head;
                    int startBlock = track * sectorsPerTrack;
                    for (int s = 0; s < sectorsPerTrack; ++s) {
                        int block = startBlock + s;
                        if (block < static_cast<int>(config.zeroFilledBlocks.size()) &&
                            !config.zeroFilledBlocks[block]) {
                            cylHasData = true;
                            break;
                        }
                    }
                }
                if (!cylHasData) {
                    m_progress.trackStatuses[cyl * 2] = TrackStatus::Skipped;
                    m_progress.trackStatuses[cyl * 2 + 1] = TrackStatus::Skipped;
                }
            }
        }
    }

    args.push_back(adfPath);

    std::string cmdForLog = "gw write";
    for (size_t i = 1; i < args.size(); ++i)
        cmdForLog += " " + args[i];
    Logger::Info("GreaseweazelWriter: Executing: {}", cmdForLog);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputLog += "$ " + cmdForLog + "\n";
        m_progress.statusMessage = "Starting gw write...";
    }
    m_state.store(WriteState::Writing);

    int exitCode = RunGwProcess(args, [this](const std::string& line) {
        // Detect verify phase from output (gw prints "Verifying" before re-reading tracks).
        if (line.find("Verif") != std::string::npos)
            m_state.store(WriteState::Verifying);
        ParseOutputLine(line);
    });

    if (exitCode < 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = "Error: Failed to launch gw command";
        m_outputLog += "Error: Failed to launch gw command\n";
        m_state.store(WriteState::Error);
        return;
    }

    if (m_state.load() == WriteState::Cancelled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.statusMessage = "Write cancelled by user";
        m_outputLog += "\nWrite cancelled by user.\n";
        // State already set to Cancelled
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        bool failed = (exitCode != 0) || m_outputHasError;

        if (!failed) {
            m_progress.statusMessage = "Write complete!";
            m_progress.progressPercent = 1.0f;

            // Mark all remaining pending/writing tracks as written/verified
            bool verified = (m_state.load() == WriteState::Verifying);
            auto doneStatus = verified ? TrackStatus::Verified : TrackStatus::Written;
            for (auto& ts : m_progress.trackStatuses) {
                if (ts == TrackStatus::Writing || ts == TrackStatus::Verifying ||
                    ts == TrackStatus::Pending) {
                    ts = doneStatus;
                }
            }

            m_state.store(WriteState::Done);
        } else {
            if (exitCode != 0) {
                m_progress.statusMessage = fmt::format("Error: gw exited with code {}", exitCode);
            } else {
                m_progress.statusMessage = "Error: gw reported failure in output";
            }
            m_outputLog += fmt::format("\ngw exited with code {} (error detected: {})\n", exitCode,
                                       m_outputHasError ? "yes" : "no");

            // Mark all non-completed tracks as failed
            for (auto& ts : m_progress.trackStatuses) {
                if (ts == TrackStatus::Pending || ts == TrackStatus::Writing ||
                    ts == TrackStatus::Verifying) {
                    ts = TrackStatus::Failed;
                }
            }

            m_state.store(WriteState::Error);
        }
    }

    Logger::Info("GreaseweazelWriter: Finished with exit code {}", exitCode);
}

} // namespace core
} // namespace adm
