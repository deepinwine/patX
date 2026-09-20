#include "patx/log.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace patx {

namespace {
std::mutex g_log_mutex;
FILE* g_log_file = nullptr;
LogLevel g_min_level = LogLevel::Info;
} // namespace

void InitLogging(const std::string& log_file_path, LogLevel min_level) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_min_level = min_level;
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
    if (!log_file_path.empty()) {
        g_log_file = fopen(log_file_path.c_str(), "a");
    }
}

void ShutDownLogging() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}

std::string LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

void LogWrite(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(g_min_level)) return;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    time_t now = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    std::string line = std::string(ts) + " [" + LogLevelName(level) + "] " + message + "\n";
    fputs(line.c_str(), stderr);
    if (g_log_file) {
        fputs(line.c_str(), g_log_file);
        fflush(g_log_file);
    }
}

std::string MaskSecret(const std::string& secret) {
    if (secret.size() <= 8) return "****";
    return secret.substr(0, 4) + "****" + secret.substr(secret.size() - 4);
}

} // namespace patx
