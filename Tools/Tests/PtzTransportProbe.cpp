// Test-only command sender: destination is hardcoded IPv4 loopback. No camera
// address, production credentials or auto-detection path can be supplied.
#include "ReolinkClient.h"
#include <Log.h>
#include <cstdlib>
#include <iostream>

int main(int Argc, char** Argv) {
    if (Argc != 3) return 2;
    char* End = nullptr;
    const auto Port = std::strtol(Argv[1], &End, 10);
    if (!End || *End || Port < 1 || Port > 65535) return 2;
    const std::string Operation = Argv[2];
    if (Operation != "move" && Operation != "stop" && Operation != "zoom" && Operation != "preset") return 2;
    Witness::LogInit("", Witness::LogLevel::Off, Witness::LogLevel::Off);
    try {
        ReolinkClient Client("127.0.0.1", static_cast<int>(Port), false, "fixture", "fixture");
        const bool Accepted = Operation == "move" ? Client.PtzControl(PtzOp::Left, 1) :
            Operation == "stop" ? Client.PtzStop() :
            Operation == "zoom" ? Client.PtzControl(PtzOp::ZoomInc, 1) : Client.PtzGoToPreset(1);
        std::cout << "acknowledged=" << Accepted << '\n';
        Witness::LogShutdown();
        return Accepted ? 0 : 3;
    } catch (...) { Witness::LogShutdown(); return 4; }
}
