#pragma once

#include <iostream>
#include <filesystem>
#include <assert.h>
#include <locale>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <sstream>

#if !defined(_WINDOWS)
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Legacy macros - now no-ops after string migration to std::string
#define _T(x) x
#define U(x) x

using StringT = std::string;
using CharT = char;
using StringStreamT = std::stringstream;

inline uint64_t GetUnixTimestamp()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch() ).count());
}

void SetStdinEcho( bool Enable );
std::string ReadFileToString( std::string Filename );

enum class StringTrim
{
	DoNotTrim,
	Trim,
};

enum class StringStrip
{
	DoNotRemoveEmpty,
	RemoveEmpty,
};

std::string Trim( std::string tInput );
std::vector< std::string > SplitString( std::string tInput, std::string tSeparator, StringTrim eTrim = StringTrim::Trim, StringStrip eStrip = StringStrip::RemoveEmpty );

std::string StringPrintfA(const char* Message, ...);

// StringToAnsi is now a no-op identity for std::string, kept for migration compatibility
inline const std::string& StringToAnsi(const std::string& str) { return str; }

template<typename T>
bool GetSettingsField( const std::unordered_map< std::string, std::string >& Settings, const char* FieldName, T& ValueOut, std::string& Errors );