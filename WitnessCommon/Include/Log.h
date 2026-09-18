#pragma once

#ifdef COMMON_EXPORTS
	#define COMMON_API __declspec(dllexport)
#else
	#define COMMON_API __declspec(dllimport)
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
	Error   = 3,
	Off     = 4
};

// A non-owning observer used by interactive presenters. File logging remains
// authoritative; observers must copy strings before returning and must not log.
using LogObserver = void (*)( void* context, LogLevel level,
	const char* timestamp, const char* message );

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

// Install or clear the single process-local observer. Passing nullptr clears it.
COMMON_API void LogSetObserver( LogObserver observer, void* context );
COMMON_API void LogSetConsoleLevel( LogLevel level );

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
