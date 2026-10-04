// MANUAL ONLY: brief low-speed movement, independently timed Stop, HTTP fallback.
// Never included in a deployable package or ordinary test invocation.
#include "ReolinkClient.h"
#include <ReolinkBaichuanClient.h>
#include <Log.h>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
static std::string Environment(const char* Name) {
#ifdef _WIN32
    char* Value = nullptr; size_t Length = 0;
    if (_dupenv_s(&Value, &Length, Name) != 0) return {};
    const std::unique_ptr<char, decltype(&std::free)> Owned(Value, &std::free);
    return Value ? std::string(Value) : std::string{};
#else
    const auto Value = std::getenv(Name); return Value ? Value : "";
#endif
}

int main() {
    const auto Camera = Environment("WITNESS_PTZ_PHYSICAL_CONFIRM");
    const auto Host = Environment("WITNESS_PTZ_PROBE_HOST");
    const auto Username = Environment("WITNESS_PTZ_PROBE_USERNAME");
    const auto Password = Environment("WITNESS_PTZ_PROBE_PASSWORD");
    const auto PortText = Environment("WITNESS_PTZ_PROBE_PORT");
    if ((Camera != "10" && Camera != "12") || Host.empty() || Username.empty() || Password.empty()) return 2;
    char* End = nullptr;
    const auto Port = std::strtol(PortText.c_str(), &End, 10);
    if (!End || *End || Port < 1 || Port > 65535) return 2;
    Witness::LogInit("", Witness::LogLevel::Off, Witness::LogLevel::Off);
    try {
        const auto Backup = ReolinkClient::AutoDetect(Host, static_cast<int>(Port), Username, Password);
        if (!Backup || !Backup->PtzStop()) {
            std::cout << "preflightHttpStop=0\n"; Witness::LogShutdown(); return 3;
        }
        Witness::Camera::ReolinkBaichuanClient Client(Host, 9000, Username, Password);
        Client.Start();
        const auto Deadline = std::chrono::steady_clock::now() + 10s;
        while (!Client.IsConnected() && std::chrono::steady_clock::now() < Deadline)
            std::this_thread::sleep_for(10ms);
        if (!Client.IsConnected() || !Client.PtzStop()) {
            Client.Stop(); std::cout << "preflightHttpStop=1 preflightBaichuanStop=0\n";
            Witness::LogShutdown(); return 3;
        }
        const auto Started = std::chrono::steady_clock::now();
        struct StopEvidence { bool Baichuan = false, Http = false; long long DispatchMs = 0, AckMs = 0; };
        // Arm the Stop before attempting movement; it does not wait on move ACK.
        auto Stop = std::async(std::launch::async, [&] {
            std::this_thread::sleep_until(Started + 250ms);
            StopEvidence Evidence;
            Evidence.DispatchMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Started).count();
            for (int Attempt = 0; Attempt < 3 && !Evidence.Baichuan && !Evidence.Http; ++Attempt) {
                try { Evidence.Baichuan = Client.PtzStop(); } catch (...) {}
                if (!Evidence.Baichuan) try { Evidence.Http = Backup->PtzStop(); } catch (...) {}
            }
            Evidence.AckMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Started).count();
            return Evidence;
        });
        bool MoveAck = false;
        try { MoveAck = Client.PtzControl("Left", 1); } catch (...) {}
        const auto Evidence = Stop.get();
        Client.Stop();
        std::cout << "preflightHttpStop=1 preflightBaichuanStop=1 moveAck=" << MoveAck
            << " baichuanStopAck=" << Evidence.Baichuan << " httpFallbackStopAck=" << Evidence.Http
            << " stopDispatchMs=" << Evidence.DispatchMs << " stopAckMs=" << Evidence.AckMs << '\n';
        Witness::LogShutdown();
        return (Evidence.Baichuan || Evidence.Http) ? (MoveAck ? 0 : 4) : 5;
    } catch (...) { Witness::LogShutdown(); return 6; }
}
