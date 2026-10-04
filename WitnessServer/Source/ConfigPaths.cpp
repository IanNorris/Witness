#include "ConfigPaths.h"
#include <cstdlib>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
namespace {
std::filesystem::path EnvironmentPath(const wchar_t* Name)
{
    wchar_t* Buffer = nullptr;
    size_t Size = 0;
    const auto Error = _wdupenv_s(&Buffer, &Size, Name);
    const std::unique_ptr<wchar_t, decltype(&std::free)> Owned(Buffer, &std::free);
    if (Error) throw std::runtime_error("Unable to read Witness data-root environment");
    return Buffer && *Buffer ? std::filesystem::path(Buffer) : std::filesystem::path{};
}
}
#endif

namespace Witness::Configuration {
std::filesystem::path ResolveFile(const std::filesystem::path& DefaultRoot,
    const std::filesystem::path& OverrideRoot, const std::string& Filename)
{
    if (Filename.empty() || Filename == "." || Filename == ".." ||
        Filename.find_first_of("/\\:") != std::string::npos || Filename.find('\0') != std::string::npos)
        throw std::invalid_argument("Witness configuration filename must be a single path component");
    const auto Root = (OverrideRoot.empty() ? DefaultRoot : OverrideRoot).lexically_normal();
    if (!Root.is_absolute() || Root == Root.root_path())
        throw std::invalid_argument("Witness data root must be an absolute dedicated directory");
    return Root / Filename;
}
}

std::filesystem::path GetConfigFilePath(std::string Filename)
{
    std::filesystem::path DefaultRoot, OverrideRoot;
#ifdef _WIN32
    OverrideRoot = EnvironmentPath(L"WITNESS_DATA_ROOT");
    if (OverrideRoot.empty()) {
        const auto Profile = EnvironmentPath(L"ProgramData");
        if (Profile.empty()) throw std::runtime_error("ProgramData is unavailable; set WITNESS_DATA_ROOT explicitly");
        DefaultRoot = Profile / L"Witness";
    }
#else
    if (const auto* Override = std::getenv("WITNESS_DATA_ROOT"); Override && *Override)
        OverrideRoot = Override;
    if (OverrideRoot.empty()) {
        const auto* Profile = std::getenv("HOME");
        if (!Profile || !*Profile) throw std::runtime_error("HOME is unavailable; set WITNESS_DATA_ROOT explicitly");
        DefaultRoot = std::filesystem::path(Profile) / ".Witness";
    }
#endif
    const auto File = Witness::Configuration::ResolveFile(DefaultRoot, OverrideRoot, Filename);
    std::filesystem::create_directories(File.parent_path());
    return File;
}
