#include "Log.h"

#include <cstdio>
#include <ctime>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <algorithm>

namespace Witness
{

static std::mutex s_LogMutex;
static FILE* s_LogFile = nullptr;
static std::string s_LogDirectory;
static std::string s_CurrentLogDate;
static LogLevel s_ConsoleLevel = LogLevel::Warning;
static LogLevel s_FileLevel = LogLevel::Debug;
static int s_RetentionDays = 30;

static const char* LevelToString( LogLevel level )
{
	switch( level )
	{
		case LogLevel::Debug:   return "DEBUG";
		case LogLevel::Info:    return "INFO";
		case LogLevel::Warning: return "WARN";
		case LogLevel::Error:   return "ERROR";
		default:                return "?";
	}
}

static void GetTimestamp( char* buf, size_t bufSize, char* dateBuf, size_t dateSize )
{
	auto now = std::chrono::system_clock::now();
	auto time_t = std::chrono::system_clock::to_time_t( now );
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( now.time_since_epoch() ) % 1000;

	struct tm tm_buf;
#ifdef _WIN32
	localtime_s( &tm_buf, &time_t );
#else
	localtime_r( &time_t, &tm_buf );
#endif

	snprintf( buf, bufSize, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
		tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
		tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count() );

	snprintf( dateBuf, dateSize, "%04d-%02d-%02d",
		tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday );
}

static void RotateLogFile( const char* currentDate )
{
	if( s_LogDirectory.empty() )
		return;

	std::string dateStr( currentDate );

	// If date changed, close old file and open new one
	if( s_CurrentLogDate != dateStr )
	{
		if( s_LogFile )
		{
			fclose( s_LogFile );
			s_LogFile = nullptr;
		}
		s_CurrentLogDate = dateStr;
	}

	if( !s_LogFile )
	{
		std::filesystem::create_directories( s_LogDirectory );
		std::string path = s_LogDirectory + "/witness-" + dateStr + ".log";
#ifdef _WIN32
		fopen_s( &s_LogFile, path.c_str(), "a" );
#else
		s_LogFile = fopen( path.c_str(), "a" );
#endif
	}
}

static void PurgeOldLogs()
{
	if( s_LogDirectory.empty() || s_RetentionDays <= 0 )
		return;

	try
	{
		auto cutoff = std::chrono::system_clock::now() - std::chrono::hours( 24 * s_RetentionDays );
		auto cutoffTime = std::chrono::system_clock::to_time_t( cutoff );

		for( auto& entry : std::filesystem::directory_iterator( s_LogDirectory ) )
		{
			if( !entry.is_regular_file() )
				continue;

			auto filename = entry.path().filename().string();
			if( filename.find( "witness-" ) != 0 || filename.find( ".log" ) == std::string::npos )
				continue;

			auto lastWrite = std::filesystem::last_write_time( entry );
			auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
				lastWrite - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
			);
			if( std::chrono::system_clock::to_time_t( sctp ) < cutoffTime )
			{
				std::filesystem::remove( entry.path() );
			}
		}
	}
	catch( ... )
	{
		// Don't let log purging errors crash the app
	}
}

void LogInit( const std::string& logDirectory, LogLevel consoleLevel, LogLevel fileLevel, int retentionDays )
{
	std::lock_guard<std::mutex> lock( s_LogMutex );
	s_LogDirectory = logDirectory;
	s_ConsoleLevel = consoleLevel;
	s_FileLevel = fileLevel;
	s_RetentionDays = retentionDays;

	PurgeOldLogs();
}

void LogShutdown()
{
	std::lock_guard<std::mutex> lock( s_LogMutex );
	if( s_LogFile )
	{
		fclose( s_LogFile );
		s_LogFile = nullptr;
	}
}

void Log( LogLevel level, const char* fmt, ... )
{
	char timestamp[32];
	char dateStr[16];
	GetTimestamp( timestamp, sizeof( timestamp ), dateStr, sizeof( dateStr ) );

	// Format the user message
	char msgBuf[2048];
	va_list args;
	va_start( args, fmt );
	vsnprintf( msgBuf, sizeof( msgBuf ), fmt, args );
	va_end( args );

	// Strip trailing newline if present (we add our own)
	size_t len = strlen( msgBuf );
	while( len > 0 && ( msgBuf[len - 1] == '\n' || msgBuf[len - 1] == '\r' ) )
		msgBuf[--len] = '\0';

	const char* levelStr = LevelToString( level );

	std::lock_guard<std::mutex> lock( s_LogMutex );

	// Console output (errors and warnings by default)
	if( level >= s_ConsoleLevel )
	{
		FILE* stream = ( level >= LogLevel::Warning ) ? stderr : stdout;
		fprintf( stream, "[%s] [%s] %s\n", timestamp, levelStr, msgBuf );
		fflush( stream );
	}

	// File output (all levels by default)
	if( level >= s_FileLevel && !s_LogDirectory.empty() )
	{
		RotateLogFile( dateStr );
		if( s_LogFile )
		{
			fprintf( s_LogFile, "[%s] [%s] %s\n", timestamp, levelStr, msgBuf );
			fflush( s_LogFile );
		}
	}
}

}
