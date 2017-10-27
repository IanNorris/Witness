#pragma once

#include <iostream>
#include <filesystem>
#include <tchar.h>
#include <assert.h>

#if !defined(_WINDOWS)
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(UNICODE) || defined(_UNICODE)
#define tcout wcout
#define tcerr wcerr
#else
#define tcout cout
#define tcerr cerr
#endif

