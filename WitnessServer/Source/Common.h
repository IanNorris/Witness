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


using CharT = char;

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


template<typename T>
bool GetSettingsField( const std::unordered_map< std::string, std::string >& Settings, const char* FieldName, T& ValueOut, std::string& Errors );