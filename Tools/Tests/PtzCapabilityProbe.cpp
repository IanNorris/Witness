// Read-only production HTTP transport check. No movement, presets, zoom writes,
// reboot or calibration calls are reachable from this executable.
#include "ReolinkClient.h"
#include <Log.h>
#include <chrono>
#include <cstdlib>
#include <iostream>

static std::string Environment(const char* Name)
{
#ifdef _WIN32
    char* Value = nullptr;
    size_t Length = 0;
    if (_dupenv_s(&Value, &Length, Name) != 0) return {};
    const std::unique_ptr<char, decltype(&std::free)> Owned(Value, &std::free);
    return Value ? std::string(Value) : std::string{};
#else
    const auto Value = std::getenv(Name);
    return Value ? std::string(Value) : std::string{};
#endif
}

int main()
{
    const auto Host = Environment("WITNESS_PTZ_PROBE_HOST");
    const auto Username = Environment("WITNESS_PTZ_PROBE_USERNAME");
    const auto Password = Environment("WITNESS_PTZ_PROBE_PASSWORD");
    const auto PortText = Environment("WITNESS_PTZ_PROBE_PORT");
    if (Host.empty() || Username.empty() || Password.empty() || PortText.empty()) return 2;
    char* End = nullptr;
    const auto Port = std::strtol(PortText.c_str(), &End, 10);
    if (!End || *End || Port < 1 || Port > 65535) return 2;
    Witness::LogInit("", Witness::LogLevel::Off, Witness::LogLevel::Off);
    const auto Started = std::chrono::steady_clock::now();
    try {
        const auto Client = ReolinkClient::AutoDetect(Host, static_cast<int>(Port), Username, Password);
        if (!Client) { std::cout << "authenticated=0\n"; Witness::LogShutdown(); return 3; }
        const auto Position = Client->GetPosition();
        const auto Zoom = Client->GetZoomFocus();
        const auto Presets = Client->GetPresets();
        size_t Enabled = 0;
        for (const auto& Preset : Presets) if (Preset.Enabled) ++Enabled;
        std::cout << "authenticated=1 positionValid=" << Position.Valid
            << " zoomValid=" << Zoom.Valid << " zoomMin=" << Zoom.ZoomMin
            << " zoomMax=" << Zoom.ZoomMax << " zoomPosition=" << Zoom.ZoomPos
            << " presetCount=" << Presets.size() << " enabledPresets=" << Enabled
            << " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - Started).count() << '\n';
        Witness::LogShutdown();
        return 0;
    } catch (...) { Witness::LogShutdown(); return 4; }
}
