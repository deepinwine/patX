// patX unified logging. Writes to a log file next to the database and to stderr.
// Callers must never pass credentials (e.g. the USPTO API key) to these macros;
// use patx::MaskSecret() when a value containing a secret has to be logged.
#pragma once

#include <string>

namespace patx {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

// Initialize logging. log_dir may be empty to disable the file sink.
// The file is named patx.log and is truncated on SetLogFile(""), kept appended
// across runs so sync/import history stays inspectable.
void InitLogging(const std::string& log_file_path, LogLevel min_level = LogLevel::Info);
void ShutDownLogging();

void LogWrite(LogLevel level, const std::string& message);
std::string LogLevelName(LogLevel level);

// Redacts the middle of a secret so logs can safely include a hint of it.
std::string MaskSecret(const std::string& secret);

} // namespace patx

#define PATX_LOG_DEBUG(msg) ::patx::LogWrite(::patx::LogLevel::Debug, (msg))
#define PATX_LOG_INFO(msg)  ::patx::LogWrite(::patx::LogLevel::Info,  (msg))
#define PATX_LOG_WARN(msg)  ::patx::LogWrite(::patx::LogLevel::Warn,  (msg))
#define PATX_LOG_ERROR(msg) ::patx::LogWrite(::patx::LogLevel::Error, (msg))
