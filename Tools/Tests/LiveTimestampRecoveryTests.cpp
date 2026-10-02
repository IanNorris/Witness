// Deterministic offline mux-reset integration test. Supply a CFR, no-B-frame,
// video-only fixture (20 fps) and an output prefix. No camera credentials needed.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void RejectAfterCooldown(LiveOutputStream& Live) {
        const int64_t NowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - Live._PacketDiagEpoch).count();
        Live._NormalizeNoBFrameTimestamps = false;
        Live._TimestampNormalizationRejected = true;
        Live._TimestampRequalification.Reject(NowMs - 60000);
        Live._DiagTimestampRejected = true;
    }
};
}
static uint64_t Timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    InputStreamSetup Setup;
    Setup.GetTimestamp = Timestamp;
    Setup.PassthroughOnly = true;
    Setup.ExportMotionVectors = false;
    Setup.HistoricalPacketBufferSeconds = 0;
    InputStream Input(Setup, 8, nullptr, argv[1]);
    if (Input.Initialize() != CameraStreamError::Success) return 3;
    LiveOutputStream Live("", &Input, 1);
    Live.SetObservedTimestampRegressionRepairAllowed(true);
    SegmentBuffer Init;
    std::ofstream Output;
    int Generation = -1, Discontinuities = 0, Samples = 0;
    bool Started = false;
    Live.SetEventCallback([&](const LiveStreamEvent& Event) {
        if (Event.EventType == LiveStreamEvent::Discontinuity) ++Discontinuities;
        if (Event.EventType == LiveStreamEvent::InitSegmentReady) {
            Init = Event.Data;
            if (Output.is_open()) Output.close();
            Generation = Event.Generation;
            Output.open(std::string(argv[2]) + "-" + std::to_string(Generation) + ".mp4", std::ios::binary);
            Started = false;
        }
        if (Event.EventType != LiveStreamEvent::PartialReady || !Event.Data) return;
        if (!Started) {
            if (!Event.Independent || !Init) return;
            Output.write(reinterpret_cast<const char*>(Init->data()), Init->size());
            Started = true;
        }
        Output.write(reinterpret_cast<const char*>(Event.Data->data()), Event.Data->size());
        ++Samples;
    });
    const auto Start = std::chrono::steady_clock::now();
    for (int Frame = 0; Frame < 380; ++Frame) {
        std::this_thread::sleep_until(Start + std::chrono::milliseconds(Frame * 50));
        if (Input.ProcessFrame(nullptr, nullptr, &Live) != CameraStreamError::Success) return 4;
        if (Frame == 40) LiveTimestampRecoveryTestAccess::RejectAfterCooldown(Live);
    }
    const auto Stats = Live.GetStreamingDiagnostics(false);
    if (Generation != 1 || Discontinuities != 1 || Stats.TimestampRetryAttempts != 1 ||
        Stats.TimestampNormalizationRejected || Samples < 50 || !Output.good()) {
        std::cerr << "FAIL generation=" << Generation << " discontinuities=" << Discontinuities
            << " attempts=" << Stats.TimestampRetryAttempts << " rejected=" << Stats.TimestampNormalizationRejected << '\n';
        return 5;
    }
    std::cout << "PASS: rejected preview earns fresh evidence, resets at a keyframe, publishes discontinuity/init and resumes fragments\n";
}
