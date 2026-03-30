#pragma once

#if defined(_WIN32)
# ifdef COMMON_EXPORTS
#  define COMMON_API __declspec(dllexport)
# else
#  define COMMON_API __declspec(dllimport)
# endif
#else
# ifdef COMMON_EXPORTS
#  define COMMON_API __attribute__((visibility("default")))
# else
#  define COMMON_API
# endif
#endif

#include <cstdarg>
#include <string>

namespace Witness
{

enum class LogLevel
{
	Debug   = 0,
	Info    = 1,
	Warning = 2,
	Error   = 3
};

// Initialize the logging system. Call once at startup.
// logDirectory: where to write log files (empty = no file logging)
// consoleLevel: minimum level shown on console (default: Warning)
// fileLevel: minimum level written to file (default: Debug)
// retentionDays: how many days of log files to keep (default: 30)
COMMON_API void LogInit( const std::string& logDirectory,
						  LogLevel consoleLevel = LogLevel::Warning,
						  LogLevel fileLevel = LogLevel::Debug,
						  int retentionDays = 30 );

// Shut down the logging system. Flushes and closes the log file.
COMMON_API void LogShutdown();

// Core logging function (printf-style)
COMMON_API void Log( LogLevel level, const char* fmt, ... );

// Get the current log directory path
COMMON_API std::string LogGetDirectory();

// Convenience macros
#define LOG_DEBUG( fmt, ... )   ::Witness::Log( ::Witness::LogLevel::Debug,   fmt, ##__VA_ARGS__ )
#define LOG_INFO( fmt, ... )    ::Witness::Log( ::Witness::LogLevel::Info,    fmt, ##__VA_ARGS__ )
#define LOG_WARNING( fmt, ... ) ::Witness::Log( ::Witness::LogLevel::Warning, fmt, ##__VA_ARGS__ )
#define LOG_ERROR( fmt, ... )   ::Witness::Log( ::Witness::LogLevel::Error,   fmt, ##__VA_ARGS__ )

}
