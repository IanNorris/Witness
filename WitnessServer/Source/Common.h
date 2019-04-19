#pragma once

#include <iostream>
#include <filesystem>
#include <tchar.h>
#include <assert.h>

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

using namespace std;
using namespace web;
using namespace utility;

void SetStdinEcho( bool Enable );
string ReadFileToString( string_t Filename );

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

string_t Trim( string_t tInput );
vector< string_t > SplitString( string_t tInput, string_t tSeparator, StringTrim eTrim = StringTrim::Trim, StringStrip eStrip = StringStrip::RemoveEmpty );

string Trim( string tInput );
vector< string > SplitString( string tInput, string tSeparator, StringTrim eTrim = StringTrim::Trim, StringStrip eStrip = StringStrip::RemoveEmpty );

string StringPrintfA(char* Message, ...);
wstring StringPrintfW(wchar_t* Message, ...);

#if defined(UNICODE) || defined(_UNICODE)
#define StringPrintfT StringPrintfW
#else
#define StringPrintfT StringPrintfA
#endif

template<typename T>
bool GetJsonField( const web::json::value& Object, const TCHAR* FieldName, T& ValueOut, utility::string_t& Errors );
