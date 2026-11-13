#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Subprocess.h"

namespace adm {
namespace core {

enum class WriteState { Idle, Starting, Writing, Verifying, Done, Error, Cancelled };

enum class TrackStatus { Pending, Writing, Written, Verifying, Verified, Failed, Skipped };

struct WriteConfig
{
    std::string adfPath;
    std::string drive = "A";
    std::string format = "amiga.amigados";
    bool verify = true;
    bool fastWrite = false;
    bool preErase = false;
    bool eraseEmpty = false;
    std::vector<bool> zeroFilledBlocks;
    // If the source is a container (ZIP/DMS), the raw ADF bytes to write to a temp file
    std::vector<uint8_t> adfData;
};

struct WriteProgress
{
    int currentTrack = 0;
    int totalTracks = 160;
    float progressPercent = 0.0f;
    std::string statusMessage;
    std::vector<TrackStatus> trackStatuses;
};

class GreaseweazelWriter
{
public:
    GreaseweazelWriter() = default;
    ~GreaseweazelWriter();

    GreaseweazelWriter(const GreaseweazelWriter&) = delete;
    GreaseweazelWriter& operator=(const GreaseweazelWriter&) = delete;

    void Start(const WriteConfig& config);
    void RunInfo();
    void RunBandwidth();
    void RunRPM(const std::string& drive);
    void Cancel();

    WriteState GetState() const { return m_state.load(); }
    WriteProgress GetProgress() const;
    std::string GetOutput() const;

private:
    void RunWrite(WriteConfig config);
    std::string BuildTrackRanges(const std::vector<bool>& zeroFilledBlocks,
                                 int sectorsPerTrack) const;
    void ParseOutputLine(const std::string& line);

    // Spawn `gw` with the given argv (args[0] is the program name) and stream its combined
    // stdout+stderr to onLine() one line at a time. Thin wrapper over m_proc; returns the child
    // exit code, or -1 if the process could not be launched.
    int RunGwProcess(const std::vector<std::string>& args,
                     const std::function<void(const std::string&)>& onLine);

    std::atomic<WriteState> m_state{WriteState::Idle};
    mutable std::mutex m_mutex;
    WriteProgress m_progress;
    std::string m_outputLog;
    bool m_outputHasError = false; // detected "Command Failed" or similar in gw output
    std::string m_tempFilePath;
    std::thread m_thread;
    Subprocess m_proc;

    void CleanupTempFile();
};

} // namespace core
} // namespace adm
