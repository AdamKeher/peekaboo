#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace adm {
namespace core {

/**
 * @brief Lifecycle state of a single background download.
 */
enum class DownloadState { Connecting, Downloading, Done, Error, Canceled };

/**
 * @brief Thread-safe handle to an in-flight (or finished) download.
 *
 * The worker thread updates the atomics live; consumers (the UI thread) read
 * them every frame. @ref data / @ref statusMsg / @ref error are guarded by
 * @ref mutex. A DownloadTask is always held via shared_ptr so it outlives the
 * owning document if that document is closed mid-download.
 */
struct DownloadTask
{
    std::atomic<DownloadState> state{DownloadState::Connecting};
    std::atomic<size_t> bytesReceived{0};
    std::atomic<int64_t> totalBytes{-1};  // -1 = unknown (no Content-Length)
    std::atomic<double> bytesPerSec{0.0}; // smoothed transfer rate
    std::atomic<bool> cancel{false};      // set to abort the transfer

    mutable std::mutex mutex;
    std::string statusMsg;     // human-readable status (guarded)
    std::string error;         // error detail when state == Error (guarded)
    std::vector<uint8_t> data; // received bytes; valid when state == Done (guarded)

    /// Progress in [0,1], or a negative value when the total size is unknown.
    double Progress() const
    {
        int64_t total = totalBytes.load();
        if (total <= 0)
            return -1.0;
        return static_cast<double>(bytesReceived.load()) / static_cast<double>(total);
    }
};

/**
 * @brief Describes a download to be started by @ref DownloadManager.
 */
struct DownloadRequest
{
    std::string url;
    std::string filename; // display name + used for format detection
    std::string fileType; // "" | "executable" | "adf" | "dms" | "zip" | "diskImage"
    std::string userAgent = "AmigaDiskManager/1.0";
};

/**
 * @brief Starts background downloads and hands back thread-safe progress handles.
 *
 * One standard mechanism shared by every remote source (TOSEC, the Chrome extension).
 * Built on cpr; the worker thread is detached and keeps the
 * DownloadTask alive through its captured shared_ptr.
 */
class DownloadManager
{
public:
    std::shared_ptr<DownloadTask> Start(const DownloadRequest& req);
};

} // namespace core
} // namespace adm
