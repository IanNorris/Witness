#pragma once
#include <filesystem>
#include <string>

namespace Witness::Configuration {
// Pure selection/validation; performs no filesystem writes.
std::filesystem::path ResolveFile(const std::filesystem::path& DefaultRoot,
    const std::filesystem::path& OverrideRoot, const std::string& Filename);
}

// WITNESS_DATA_ROOT overrides only database/log configuration storage. Media
// cache and TLS paths still come from settings in that database.
std::filesystem::path GetConfigFilePath(std::string Filename);
