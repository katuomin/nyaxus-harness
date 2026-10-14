#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace nyaxus {
namespace log {

// Minimal operator-facing logging for the daemon. Writes to stderr (so the
// usual `> daemon.log 2>&1` capture pattern keeps working), one line per
// event:
//   2026-09-05T22:15:33 [info] run completed run-123 (4200 ms)
// The level is read once from NYAXUS_LOG_LEVEL (debug|info|warn|error,
// default info). Callers log ids, counts, durations, and error strings,
// never secrets or full payloads.

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline Level threshold() {
    static const Level level = [] {
        const char* raw = std::getenv("NYAXUS_LOG_LEVEL");
        if (!raw) return Level::Info;
        std::string value(raw);
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (value == "debug") return Level::Debug;
        if (value == "warn" || value == "warning") return Level::Warn;
        if (value == "error") return Level::Error;
        return Level::Info;
    }();
    return level;
}

inline void emit(Level level, const std::string& message) {
    if (level < threshold()) return;
    static std::mutex mutex;
    const std::lock_guard<std::mutex> lock(mutex);
    const std::time_t now = std::time(nullptr);
    std::tm tm_buffer{};
#ifdef _WIN32
    localtime_s(&tm_buffer, &now);
#else
    localtime_r(&now, &tm_buffer);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_buffer);
    const char* tag = level == Level::Debug ? "debug"
                   : level == Level::Info ? "info"
                   : level == Level::Warn ? "warn"
                   : "error";
    std::fprintf(stderr, "%s [%s] %s\n", stamp, tag, message.c_str());
    std::fflush(stderr);
}

inline void debug(const std::string& message) { emit(Level::Debug, message); }
inline void info(const std::string& message) { emit(Level::Info, message); }
inline void warn(const std::string& message) { emit(Level::Warn, message); }
inline void error(const std::string& message) { emit(Level::Error, message); }

}  // namespace log
}  // namespace nyaxus
