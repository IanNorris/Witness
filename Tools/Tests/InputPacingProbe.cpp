// Opt-in local-file/loopback integration probe, with no production configuration.
#include "InputStream.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

static uint64_t Timestamp()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int main()
{
    const char* Source = std::getenv("WITNESS_TEST_PACING_SOURCE");
    const char* Expected = std::getenv("WITNESS_TEST_PACING_EXPECTED");
    if (!Source || !Expected || (std::string(Expected) != "0" && std::string(Expected) != "1")) return 2;
    Witness::Camera::InputStreamSetup Setup;
    Setup.GetTimestamp = Timestamp;
    Setup.PassthroughOnly = true;
    Setup.ExportMotionVectors = false;
    Setup.HistoricalPacketBufferSeconds = 0;
    Witness::Camera::InputStream Input(Setup, 1, nullptr, Source);
    if (!Input.RequiresPacketPacing()) return 3; // Conservative before opening.
    if (Input.Initialize() != Witness::Camera::CameraStreamError::Success) return 4;
    bool Actual = Input.RequiresPacketPacing();
    Input.Shutdown();
    if (!Input.RequiresPacketPacing()) return 5;
    std::cout << "requiresPacketPacing=" << Actual << '\n';
    return Actual == (std::string(Expected) == "1") ? 0 : 6;
}
