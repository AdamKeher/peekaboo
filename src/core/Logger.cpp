#include "Logger.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

bool IsTty(FILE* f)
{
#if defined(_WIN32)
    return _isatty(_fileno(f)) != 0;
#else
    return isatty(fileno(f)) != 0;
#endif
}

// Muted 256-color palette for category labels. Each entry is an ANSI SGR sequence.
// Colors chosen to be distinguishable but not garish on a dark terminal.
constexpr const char* kCatPalette[] = {
    "\033[38;5;67m",  // steel blue       RGB(95,135,175)
    "\033[38;5;71m",  // sage green       RGB(95,175,95)
    "\033[38;5;131m", // dusty rose       RGB(175,95,95)
    "\033[38;5;136m", // muted gold       RGB(175,135,0)
    "\033[38;5;140m", // lavender         RGB(175,135,215)
    "\033[38;5;72m",  // teal             RGB(95,175,135)
    "\033[38;5;179m", // tan              RGB(215,175,95)
    "\033[38;5;110m", // light steel blue RGB(135,175,215)
};
constexpr size_t kCatPaletteSize = sizeof(kCatPalette) / sizeof(kCatPalette[0]);

const char* CategoryColor(std::string_view cat)
{
    uint32_t h = 0;
    for (unsigned char c : cat)
        h = h * 31u + c;
    return kCatPalette[h % kCatPaletteSize];
}

} // namespace

namespace adm::core {

LogCallback Logger::s_callback;

// Guards s_callback. Logging happens from background threads (floppy reader, HTTP server,
// detached upload/TOSEC workers) while SetCallback() runs on the main thread — notably
// SetCallback(nullptr) during shutdown. Reassigning a std::function while another thread
// reads/invokes it is a data race, so all access goes through this mutex.
static std::mutex s_callbackMutex;

void Logger::Init()
{
    // Initialization code if needed (e.g. open file)
}

void Logger::SetCallback(LogCallback callback)
{
    std::lock_guard<std::mutex> lock(s_callbackMutex);
    s_callback = std::move(callback);
}

void Logger::LogMessage(LogLevel level, std::string_view message) { LogInternal(level, message); }

void Logger::LogInternal(LogLevel level, std::string_view message)
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    const char* prefix;
    const char* levelAnsi;
    FILE* outStream = stdout;

    switch (level) {
        case LogLevel::Info:
            prefix = "[INFO] ";
            levelAnsi = "\033[36m"; // cyan
            break;
        case LogLevel::Warning:
            prefix = "[WARN] ";
            levelAnsi = "\033[33m"; // yellow
            break;
        case LogLevel::Error:
            prefix = "[ERROR]";
            levelAnsi = "\033[1;31m"; // bold red
            outStream = stderr;
            break;
        case LogLevel::Debug:
        default:
            prefix = "[DEBUG]";
            levelAnsi = "\033[90m"; // dark gray
            break;
    }

    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    const bool color = IsTty(outStream);
    const char* reset = color ? "\033[0m" : "";
    const char* dim = color ? "\033[2m" : "";
    const char* lvl = color ? levelAnsi : "";

    // Detect "Category: detail" at the start of the message.
    // Category must be all word chars / spaces, max 40 chars, followed by ": ".
    std::string_view cat, detail;
    if (color) {
        const size_t pos = message.find(": ");
        if (pos != std::string_view::npos && pos > 0 && pos <= 40) {
            bool valid = true;
            for (size_t i = 0; i < pos && valid; ++i) {
                const unsigned char c = static_cast<unsigned char>(message[i]);
                valid = std::isalnum(c) || c == '_' || c == ' ';
            }
            if (valid) {
                cat = message.substr(0, pos);
                detail = message.substr(pos + 2);
            }
        }
    }

    if (!cat.empty()) {
        fmt::print(outStream, "{}[{:02d}:{:02d}:{:02d}]{} {}{}{} {}{}{}: {}\n", dim, tm_buf.tm_hour,
                   tm_buf.tm_min, tm_buf.tm_sec, reset, lvl, prefix, reset, CategoryColor(cat), cat,
                   reset, detail);
    } else {
        fmt::print(outStream, "{}[{:02d}:{:02d}:{:02d}]{} {}{}{} {}\n", dim, tm_buf.tm_hour,
                   tm_buf.tm_min, tm_buf.tm_sec, reset, lvl, prefix, reset, message);
    }

    // Notify callback (for LogView). Copy the callback under the lock, then invoke it
    // outside the lock so a slow/re-entrant callback can't block other loggers.
    LogCallback callback;
    {
        std::lock_guard<std::mutex> lock(s_callbackMutex);
        callback = s_callback;
    }
    if (callback) {
        callback(level, std::string(message));
    }
}

} // namespace adm::core
