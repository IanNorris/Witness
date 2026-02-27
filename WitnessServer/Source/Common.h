#pragma once

#include <iostream>
#include <filesystem>
#include <tchar.h>
#include <assert.h>
#include <locale>
#include <codecvt>
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

#if defined(UNICODE) || defined(_UNICODE)
#define tcout wcout
#define tcerr wcerr
#define tcin wcin
#define tstrcmp wcscmp
#define tstricmp wcsicmp
#else
#define tcout cout
#define tcerr cerr
#define tcin cin
#define tstrcmp strcmp
#define tstricmp stricmp
#endif

// String types - currently wide strings (legacy from CppREST SDK).
// TODO: Migrate to std::string in Phase 2 Step 9.
#if defined(UNICODE) || defined(_UNICODE)
using StringT = std::wstring;
using CharT = wchar_t;
using StringStreamT = std::wstringstream;
#define U(x) _T(x)
#else
using StringT = std::string;
using CharT = char;
using StringStreamT = std::stringstream;
#define U(x) x
#endif

inline uint64_t GetUnixTimestamp()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch() ).count());
}

void SetStdinEcho( bool Enable );
std::string ReadFileToString( StringT Filename );

///Enum to determine whether a string should be trimmed when being split
enum class StringTrim
{
	DoNotTrim,  //!< Do not trim the string
	Trim,		//!< Trim whitespace from either side of a string when splitting
};

///Enum to determine if a split string that contains character should be added to the resulting list
enum class StringStrip
{
	DoNotRemoveEmpty,  //!< Keep empty strings
	RemoveEmpty,		//!< Remove empty strings
};

StringT Trim( StringT tInput );
std::vector< StringT > SplitString( StringT tInput, StringT tSeparator, StringTrim eTrim = StringTrim::Trim, StringStrip eStrip = StringStrip::RemoveEmpty );

std::string Trim(std::string tInput );
std::vector< std::string > SplitString(std::string tInput, std::string tSeparator, StringTrim eTrim = StringTrim::Trim, StringStrip eStrip = StringStrip::RemoveEmpty );

std::string StringPrintfA(const char* Message, ...);
std::wstring StringPrintfW(const wchar_t* Message, ...);

#if defined(UNICODE) || defined(_UNICODE)
#define StringPrintfT StringPrintfW
#else
#define StringPrintfT StringPrintfA
#endif

template<typename T>
bool GetSettingsField( const std::unordered_map< StringT, StringT >& Settings, const TCHAR* FieldName, T& ValueOut, StringT& Errors );

std::string StringToAnsi(const std::wstring& wstr);