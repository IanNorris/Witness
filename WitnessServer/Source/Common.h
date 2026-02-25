#pragma once

#include <iostream>
#include <filesystem>
#include <tchar.h>
#include <assert.h>
#include <locale>
#include <codecvt>
#include <string>

#include "cpprest/json.h"

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

using namespace web;
using namespace utility;

// Our own string types - currently wide strings to match CppREST SDK.
// When CppREST is replaced, change these to std::string/char/std::stringstream.
using StringT = utility::string_t;
using CharT = utility::char_t;
using StringStreamT = utility::stringstream_t;

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
bool GetSettingsField( const std::unordered_map< StringT, StringT >& Settings, const TCHAR* FieldName, T& ValueOut, utility::string_t& Errors );

template<typename T>
bool GetJsonField(const web::json::value& Object, const TCHAR* FieldName, T& ValueOut, utility::string_t& Errors);

std::string StringToAnsi(const std::wstring& wstr);