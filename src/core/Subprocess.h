#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/types.h> // pid_t
#endif

namespace adm {
namespace core {

/**
 * @brief Where a child runs and what it sees: a working directory (empty: the parent's) and
 *        changes to the parent's environment - a name with a value sets it, a name with none
 *        removes it. The rest of the environment is inherited.
 */
struct SubprocessOptions
{
    std::string workingDir;
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
};

/**
 * @brief Spawns a child process without a shell, capturing combined stdout+stderr.
 *
 * Cross-platform: fork/execvp on POSIX, CreateProcess on Windows. Output is streamed line by
 * line (split on '\n' and '\r' — gw uses '\r' for in-place progress) to a caller callback. The
 * running process can be terminated from another thread via Cancel(), giving the same real
 * cancellation on both platforms (POSIX kills the child's process group; Windows terminates the
 * Job Object the child and its own children run in).
 *
 * Because no shell is involved, argument strings carry no injection surface — args[0] is the
 * program name (resolved via PATH) and the remaining entries are passed through verbatim.
 */
class Subprocess
{
public:
    Subprocess() = default;
    ~Subprocess();

    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    /**
     * @brief Run args[0] with args as argv, blocking until the child exits or its output ends.
     * @param args argv vector; args[0] is the program name (looked up on PATH).
     * @param onLine Invoked on the calling thread for each complete, non-empty output line.
     * @return The child exit code, 1 if it was cancelled/killed, or -1 if it could not be launched.
     */
    int Run(const std::vector<std::string>& args,
            const std::function<void(const std::string&)>& onLine);
    // The same, in `options.workingDir` with `options.env` applied.
    int Run(const std::vector<std::string>& args,
            const std::function<void(const std::string&)>& onLine,
            const SubprocessOptions& options);

    /**
     * @brief Request termination of the running child. Safe to call from another thread, and
     *        before/after Run() (a pending cancel is honored when the next Run() starts).
     */
    void Cancel();

private:
    std::atomic<bool> m_cancelled{false};
#ifdef _WIN32
    std::atomic<void*> m_processHandle{nullptr}; // HANDLE of the running child (nullptr if none)
    // A Job Object holding the child and everything it starts: a cancel (a timeout) ends them
    // all, so a grandchild that inherited the output pipe cannot keep Run reading forever
    std::atomic<void*> m_jobHandle{nullptr};
#else
    std::atomic<pid_t> m_childPid{0}; // pid of the running child (0 if none)
#endif
};

/**
 * @brief A child process fed through its standard input - the video capture's pipe to ffmpeg.
 *
 * Like Subprocess: no shell, no console window on Windows (CREATE_NO_WINDOW), args[0] looked up
 * on PATH. Output (stdout and stderr together) goes to a log file rather than back to the caller,
 * since nothing reads it while the child runs; the caller reads the file when something fails.
 * Paths and arguments are UTF-8 on every platform.
 *
 * Not thread-safe: Start, Write and Finish belong to one thread. SIGPIPE is the caller's to
 * handle on POSIX - a Write to a child that has died must not kill the process, so the writing
 * thread blocks it (see FfmpegRecorder).
 */
class PipeProcess
{
public:
    PipeProcess() = default;
    ~PipeProcess(); // kills a child that is still running

    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;

    bool Start(const std::vector<std::string>& args, const std::filesystem::path& logFile,
               std::string& outError);
    // Blocks until the child has taken all of it. False once the child has gone.
    bool Write(const uint8_t* data, size_t size);
    // Closes the child's input and waits for it. The exit code, or -1 if it never started.
    int Finish();
    void Kill();
    bool IsRunning() const { return m_running; }

private:
    bool m_running = false;
#ifdef _WIN32
    void* m_process = nullptr; // HANDLE
    void* m_stdinWrite = nullptr;
#else
    pid_t m_pid = 0;
    int m_stdinWrite = -1;
#endif
};

} // namespace core
} // namespace adm
