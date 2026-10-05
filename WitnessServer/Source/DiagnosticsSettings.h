#pragma once

#include <cstdlib>
#include <memory>
#include <string_view>

namespace Witness::Diagnostics
{
// Callers cache the result once per process. Only exact "0" disables an
// observer; missing/invalid settings retain default-on diagnostic coverage.
inline bool ReadObserverSetting(const char* Name)
{
#ifdef _WIN32
    char* Value = nullptr;
    size_t Required = 0;
    const auto Error = _dupenv_s(&Value, &Required, Name);
    const std::unique_ptr<char, decltype(&std::free)> Owned(Value, &std::free);
    return Error != 0 || !Value || std::string_view(Value) != "0";
#else
    const char* Value = std::getenv(Name);
    return !Value || std::string_view(Value) != "0";
#endif
}
}
