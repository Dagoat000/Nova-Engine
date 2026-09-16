// File: src/Core/Logger.h
//
// Minimal logging. Debug builds log everything to both the console and the
// VS output window; Release builds compile most of it away so logging never
// shows up as overhead in a shipping frame.
#pragma once

#include <Windows.h>
#include <cstdio>
#include <string>

namespace core
{
    enum class LogLevel
    {
        Info,
        Warning,
        Error
    };

    class Logger
    {
    public:
        static void Log(LogLevel level, const std::string& message)
        {
#if defined(_DEBUG)
            const char* prefix = "[INFO] ";
            if (level == LogLevel::Warning) prefix = "[WARN] ";
            if (level == LogLevel::Error)   prefix = "[ERROR] ";

            std::string line = std::string(prefix) + message + "\n";
            OutputDebugStringA(line.c_str());
            std::fputs(line.c_str(), level == LogLevel::Error ? stderr : stdout);
#else
            // Release builds only surface errors - info/warning spam has no
            // place in a hot render loop.
            if (level == LogLevel::Error)
            {
                std::string line = "[ERROR] " + message + "\n";
                OutputDebugStringA(line.c_str());
            }
#endif
        }
    };
}

#define LOG_INFO(msg)  ::core::Logger::Log(::core::LogLevel::Info, msg)
#define LOG_WARN(msg)  ::core::Logger::Log(::core::LogLevel::Warning, msg)
#define LOG_ERROR(msg) ::core::Logger::Log(::core::LogLevel::Error, msg)
