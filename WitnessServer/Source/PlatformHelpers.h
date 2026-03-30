#pragma once

// ---------------------------------------------------------------------------
// Cross-platform helpers shared across WitnessServer source files.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Witness
{

// Returns the directory containing the current executable.
inline std::filesystem::path GetExeDir()
{
#ifdef _WIN32
	wchar_t buf[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, buf, MAX_PATH );
	return std::filesystem::path( buf ).parent_path();
#else
	return std::filesystem::canonical( "/proc/self/exe" ).parent_path();
#endif
}

} // namespace Witness
