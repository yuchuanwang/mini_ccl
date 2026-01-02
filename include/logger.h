// loggger.h
// Using fmt::format (Python f-string alike) to output logs
//

#pragma once

#include <string>
#include <cstring>
#include <ctime>
#include <fmt/core.h>
#include <fmt/format.h>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger
{
public:
    static Logger& Instance()
    {
        static Logger instance;
        return instance;
    }

    void SetLogLevel(LogLevel level)
    {
        current_level = level;
    }
    LogLevel GetLogLevel() const
    {
        return current_level;
    }

    template<typename... Args>
    void Debug(fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (current_level <= LogLevel::DEBUG)
        {
            LogFormatted(LogLevel::DEBUG, fmt::format(fmt_str, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    void Info(fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (current_level <= LogLevel::INFO)
        {
            LogFormatted(LogLevel::INFO, fmt::format(fmt_str, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    void Warn(fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (current_level <= LogLevel::WARN)
        {
            LogFormatted(LogLevel::WARN, fmt::format(fmt_str, std::forward<Args>(args)...));
        }
    }

    template<typename... Args>
    void Error(fmt::format_string<Args...> fmt_str, Args&&... args)
    {
        if (current_level <= LogLevel::ERROR)
        {
            LogFormatted(LogLevel::ERROR, fmt::format(fmt_str, std::forward<Args>(args)...));
        }
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    const char* LevelToString(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
        }
    }

    void LogFormatted(LogLevel level, const std::string& msg)
    {
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        char time_buf[64];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", timeinfo);
        fprintf(stdout, "[%s] [%s] %s\n", time_buf, LevelToString(level), msg.c_str());
        fflush(stdout);
    }

private:
    LogLevel current_level = LogLevel::INFO;
};

// Macro
#define LOG_DEBUG(...) Logger::Instance().Debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::Instance().Info(__VA_ARGS__)
#define LOG_WARN(...) Logger::Instance().Warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::Instance().Error(__VA_ARGS__)
