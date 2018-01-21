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
#define tstrcmp wcscmp
#define tstricmp wcsicmp
#else
#define tcout cout
#define tcerr cerr
#define tstrcmp strcmp
#define tstricmp stricmp
#endif

using namespace std;
using namespace web;
using namespace utility;

string ReadFileToString( string_t Filename );

template<typename T>
bool GetJsonField( const web::json::value& Object, const TCHAR* FieldName, T& ValueOut, utility::string_t& Errors );
