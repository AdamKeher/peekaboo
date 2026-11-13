#pragma once

#include <fmt/core.h>
#include <functional>
#include <string>
#include <string_view>

namespace adm::core {

enum class LogLevel { Info, Warning, Error, Debug };

using LogCallback = std::function<void(LogLevel, const std::string&)>;

class Logger
{
public:
    static void Init();

    static void SetCallback(LogCallback callback);

    // Log an already-formatted message at the requested runtime level. Unlike the
    // Debug() convenience template, Debug-level messages passed here are retained in
    // release builds so runtime data sources can use the LogView's Debug category.
    static void LogMessage(LogLevel level, std::string_view message);

    template <typename... Args> static void Info(fmt::format_string<Args...> fmt, Args&&... args)
    {
        Log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void Warn(fmt::format_string<Args...> fmt, Args&&... args)
    {
        Log(LogLevel::Warning, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void Error(fmt::format_string<Args...> fmt, Args&&... args)
    {
        Log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void Debug(fmt::format_string<Args...> fmt, Args&&... args)
    {
#ifdef NDEBUG
// Optimized out in release unless we want debug logs there too
#else
        Log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
#endif
    }

private:
    template <typename... Args>
    static void Log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args)
    {
        std::string message = fmt::format(fmt, std::forward<Args>(args)...);
        LogInternal(level, message);
    }

    static void LogInternal(LogLevel level, std::string_view message);
    static LogCallback s_callback;
};

} // namespace adm::core
