#include "DownloadManager.h"

#include <chrono>
#include <thread>

#include <cpr/cpr.h>
#include <fmt/core.h>

#include "Logger.h"

namespace adm {
namespace core {

namespace {

std::string FormatBytes(int64_t bytes)
{
    if (bytes < 0)
        return "?";
    double b = static_cast<double>(bytes);
    if (b < 1024.0)
        return fmt::format("{} B", bytes);
    if (b < 1024.0 * 1024.0)
        return fmt::format("{:.1f} KB", b / 1024.0);
    return fmt::format("{:.1f} MB", b / (1024.0 * 1024.0));
}

// Runs the transfer; returns the cpr::Response.
cpr::Response RunGet(const DownloadRequest& req, const std::shared_ptr<DownloadTask>& task)
{
    cpr::Header header;
    header["User-Agent"] = req.userAgent;

    using clock = std::chrono::steady_clock;
    auto startTime = clock::now();
    auto lastSample = startTime;
    size_t lastSampleBytes = 0;

    task->state = DownloadState::Downloading;

    return cpr::Get(
        cpr::Url{req.url}, header, cpr::ConnectTimeout{15000}, cpr::Timeout{600000},
        cpr::ProgressCallback{[&](cpr::cpr_off_t downloadTotal, cpr::cpr_off_t /*downloadNow*/,
                                  cpr::cpr_off_t /*uploadTotal*/, cpr::cpr_off_t /*uploadNow*/,
                                  intptr_t /*userdata*/) -> bool {
            if (downloadTotal > 0)
                task->totalBytes = static_cast<int64_t>(downloadTotal);
            return !task->cancel.load(); // return false → abort
        }},
        cpr::WriteCallback{[&](std::string_view data, intptr_t) -> bool {
            if (task->cancel.load())
                return false;
            {
                std::lock_guard<std::mutex> lk(task->mutex);
                task->data.insert(task->data.end(), data.begin(), data.end());
            }
            size_t received = task->bytesReceived.fetch_add(data.size()) + data.size();

            // Sample transfer rate ~4x/sec over the elapsed window.
            auto now = clock::now();
            double sinceSample = std::chrono::duration<double>(now - lastSample).count();
            if (sinceSample >= 0.25) {
                double rate = (received - lastSampleBytes) / sinceSample;
                double prev = task->bytesPerSec.load();
                // Light EMA smoothing so the number doesn't jitter.
                task->bytesPerSec = (prev > 0.0) ? (prev * 0.6 + rate * 0.4) : rate;
                lastSample = now;
                lastSampleBytes = received;

                std::lock_guard<std::mutex> lk(task->mutex);
                task->statusMsg = FormatBytes(static_cast<int64_t>(received)) + " received";
            }
            return true;
        }});
}

} // namespace

static std::string NormalizeUrl(const std::string& url)
{
    // scene.org /view/ links show a mirror-picker page; /get/ downloads directly.
    const std::string viewPrefix = "https://files.scene.org/view/";
    const std::string getPrefix = "https://files.scene.org/get/";
    if (url.substr(0, viewPrefix.size()) == viewPrefix)
        return getPrefix + url.substr(viewPrefix.size());
    return url;
}

std::shared_ptr<DownloadTask> DownloadManager::Start(const DownloadRequest& req)
{
    auto task = std::make_shared<DownloadTask>();
    {
        std::lock_guard<std::mutex> lk(task->mutex);
        task->statusMsg = "Connecting...";
    }

    DownloadRequest normalizedReq = req;
    normalizedReq.url = NormalizeUrl(req.url);

    std::thread worker([normalizedReq, task]() {
        const DownloadRequest& req = normalizedReq;
        try {
            cpr::Response resp = RunGet(req, task);

            if (task->cancel.load()) {
                task->state = DownloadState::Canceled;
                return;
            }

            if (resp.error.code != cpr::ErrorCode::OK) {
                std::lock_guard<std::mutex> lk(task->mutex);
                task->error = fmt::format("Download error: {}", resp.error.message);
                task->statusMsg = task->error;
                task->state = DownloadState::Error;
                return;
            }
            if (resp.status_code != 200) {
                std::lock_guard<std::mutex> lk(task->mutex);
                task->error = fmt::format("HTTP {}", resp.status_code);
                task->statusMsg = task->error;
                task->state = DownloadState::Error;
                return;
            }

            bool empty;
            {
                std::lock_guard<std::mutex> lk(task->mutex);
                empty = task->data.empty();
            }

            if (empty) {
                std::lock_guard<std::mutex> lk(task->mutex);
                task->error = "Server returned an empty file";
                task->statusMsg = task->error;
                task->state = DownloadState::Error;
                return;
            }

            {
                std::lock_guard<std::mutex> lk(task->mutex);
                task->statusMsg = fmt::format(
                    "{} downloaded", FormatBytes(static_cast<int64_t>(task->bytesReceived.load())));
            }
            task->state = DownloadState::Done;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(task->mutex);
            task->error = e.what();
            task->statusMsg = task->error;
            task->state = DownloadState::Error;
        }
    });
    worker.detach();

    return task;
}

} // namespace core
} // namespace adm
