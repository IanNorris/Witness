// Offline regression test: supply a >=140s, 20fps, no-B-frame video fixture.
// Virtual elapsed time lets the actual mux exercise a 15s buffered pause/burst
// at a validation boundary without waiting two minutes or using camera secrets.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include "StreamData.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <chrono>
#include <iostream>
#include <fstream>
#include <stdexcept>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void Elapsed(LiveOutputStream& Live, int64_t Ms) {
        Live._PacketDiagEpoch = std::chrono::steady_clock::now() - std::chrono::milliseconds(Ms);
    }
};
}

static LiveOutputStream::StreamingDiagnostics Run(const char* Fixture, bool Pause, const std::string& Prefix, bool Slower = false) {
    InputStreamSetup Setup;
    Setup.GetTimestamp = []() -> uint64_t { return 1; };
    Setup.PassthroughOnly = true;
    Setup.ExportMotionVectors = false;
    Setup.HistoricalPacketBufferSeconds = 0;
    InputStream Input(Setup, 9, nullptr, Fixture);
    if (Input.Initialize() != CameraStreamError::Success) throw std::runtime_error("fixture initialization");
    // This harness replaces packet timestamps, so use the camera's 90kHz
    // clock rather than the fixture MP4's encoder-selected timebase.
    Input.GetData().FormatContext->streams[Input.GetData().ChosenStreamIndex]->time_base = {1, 90000};
    LiveOutputStream Live("", &Input, 1);
    Live.SetObservedTimestampRegressionRepairAllowed(true);
    std::ofstream Output;
    SegmentBuffer Init;
    bool Started = false;
    int Discontinuities = 0;
    Live.SetEventCallback([&](const LiveStreamEvent& Event) {
        if (Event.EventType == LiveStreamEvent::Discontinuity) ++Discontinuities;
        if (Prefix.empty()) return;
        if (Event.EventType == LiveStreamEvent::InitSegmentReady) {
            Init = Event.Data;
            if (Output.is_open()) Output.close();
            Output.open(Prefix + "-" + std::to_string(Event.Generation) + ".mp4", std::ios::binary);
            Started = false;
        }
        if (Event.EventType != LiveStreamEvent::PartialReady || !Event.Data) return;
        if (!Started) {
            if (!Event.Independent || !Init) return;
            Output.write(reinterpret_cast<const char*>(Init->data()), Init->size());
            Started = true;
        }
        Output.write(reinterpret_cast<const char*>(Event.Data->data()), Event.Data->size());
    });
    AVPacket* Packet = av_packet_alloc();
    int Frames = 0;
    while (Frames < 2800 && av_read_frame(Input.GetData().FormatContext, Packet) >= 0) {
        if (Packet->stream_index == Input.GetData().ChosenStreamIndex) {
            // Distinct access units remain in order; only the declared source
            // clock has the Tapo-like one-second backward step.
            Packet->dts = Packet->pts = Frames * 4500LL - (Frames / 20) * 30000LL;
            // A camera's sawtooth can cross the newly selected keyframe epoch.
            // The payload is still the next valid dependent picture.
            if (Pause && Frames == 2645) Packet->dts = Packet->pts = 7920000 - 6750;
            Packet->duration = 4500;
            int64_t Ms = Frames * (Slower ? 55LL : 50LL);
            if (Pause && Frames >= 2300 && Ms < 130000) Ms = 130000;
            LiveTimestampRecoveryTestAccess::Elapsed(Live, Ms);
            Live.BeginDiagnosticActivity();
            if (Live.WriteInterleavedPacket(Packet) != CameraStreamError::Success)
                throw std::runtime_error("mux failure");
            ++Frames;
        }
        av_packet_unref(Packet);
    }
    av_packet_free(&Packet);
    if (Frames != 2800) throw std::runtime_error("fixture too short");
    const auto Stats = Live.GetStreamingDiagnostics(false);
    Live.SetEventCallback({});
    if ((!Prefix.empty() && !Output.good()) || Discontinuities != (Pause ? 1 : 0))
        throw std::runtime_error("output/discontinuity failure");
    return Stats;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto Control = Run(argv[1], false, std::string(argv[2]) + "-control");
    const auto Paused = Run(argv[1], true, std::string(argv[2]) + "-pause");
    const auto Mismatched = Run(argv[1], false, "", true);
    if (Control.TimestampNormalizationRejected || !Control.TimestampNormalizationActive ||
        Paused.TimestampNormalizationRejected || !Paused.TimestampNormalizationActive ||
        Control.TimestampPauseResets != 0 || Paused.TimestampPauseResets != 1 ||
        Paused.TimestampLastPauseGapMs < 15000 || Paused.TimestampRetryAttempts != 0 ||
        Control.DroppedVideoPackets != 0 || Paused.DroppedVideoPackets != 0 ||
        Control.AcceptedVideoPackets != 2799 || Paused.AcceptedVideoPackets != 2798)
        return 1;
    if (!Mismatched.TimestampNormalizationRejected || Mismatched.TimestampPauseResets != 0)
        return 1; // a sustained 10% rate mismatch still fails the original cadence guard
    std::cout << "PASS: pause/burst earns fresh cadence and resets at a clean keyframe;"
              << " no rejection or timestamp drops, one pending-frame retirement,"
              << " no cadence-retry budget consumed, both generations saved\n";
}
