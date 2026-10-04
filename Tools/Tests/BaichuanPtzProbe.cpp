// Exported DLL consumer, hardcoded loopback and synthetic credentials only.
#include <ReolinkBaichuanClient.h>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const int Port = std::stoi(argv[1]);
    if (Port < 1 || Port > 65535) return 2;
    Witness::Camera::ReolinkBaichuanClient Client("127.0.0.1", Port, "fixture", "fixture");
    Client.Start();
    const auto Deadline = std::chrono::steady_clock::now() + 10s;
    while (!Client.IsConnected() && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(5ms);
    if (!Client.IsConnected()) { Client.Stop(); return 4; }
    const std::string Operation = argv[2];
    const auto Start = std::chrono::steady_clock::now();
    bool Result = false;
    if (Operation == "stop-during-wait") {
        auto Pending = std::async(std::launch::async, [&] { return Client.PtzStop(2); });
        std::this_thread::sleep_for(150ms);
        Client.Stop();
        Result = Pending.get();
    } else if (Operation == "reconnect") {
        if (Client.PtzStop(2)) { Client.Stop(); return 6; }
        const auto ReconnectDeadline = std::chrono::steady_clock::now() + 8s;
        while (!Client.IsConnected() && std::chrono::steady_clock::now() < ReconnectDeadline)
            std::this_thread::sleep_for(5ms);
        if (!Client.IsConnected()) { Client.Stop(); return 7; }
        Result = Client.PtzStop(2);
    } else if (Operation == "preset") Result = Client.PtzGoToPreset(7, 2);
    else if (Operation == "stop" || Operation == "event") Result = Client.PtzStop(2);
    else if (Operation == "move") Result = Client.PtzControl("Left", 1, 2);
    else { Client.Stop(); return 2; }
    const auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Start).count();
    if (Operation == "event") {
        const auto State = Client.GetDetections();
        if (!State.HasData || !State.HasMotion || State.Detections.size() != 1) {
            Client.Stop(); return 5;
        }
    }
    Client.Stop();
    std::cout << "RESULT " << Result << " " << Elapsed << '\n';
    return Result ? 0 : 3;
}
