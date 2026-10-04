// Offline regression test: supply a >=140s, 20fps, no-B-frame video fixture.
// Virtual elapsed time lets the actual mux exercise a 15s buffered pause/burst
// at a validation boundary without waiting two minutes or using camera secrets.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include "LiveOutputStreamData.h"
#include "StreamData.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <chrono>
#include <cmath>
#include <iostream>
#include <fstream>
#include <stdexcept>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void Elapsed(LiveOutputStream& Live, int64_t Ms) {
        Live._LiveData->_PacketDiagEpoch = std::chrono::steady_clock::now() - std::chrono::milliseconds(Ms);
    }
};
}

static LiveOutputStream::StreamingDiagnostics Run(const char* Fixture, bool Pause, const std::string& Prefix, bool Slower = false, bool BoundedSlower = false, bool StableSource = false, bool Unstable = false) {
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
            Packet->dts = Packet->pts = Frames * 4500LL - (StableSource ? 0 : (Frames / 20) * 30000LL);
            // A camera's sawtooth can cross the newly selected keyframe epoch.
            // The payload is still the next valid dependent picture.
            if (Pause && Frames == 2645) Packet->dts = Packet->pts = 7920000 - 6750;
            Packet->duration = 4500;
            int64_t Ms = Frames * (Slower ? 55LL : BoundedSlower ? 56LL : 50LL);
            if (Unstable) {
                const int Part = Frames % 800;
                Ms = (Frames / 800) * (400 * 55LL + 400 * 75LL) +
                    (std::min)(Part, 400) * 55LL + (std::max)(Part - 400, 0) * 75LL;
            }
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
    const auto BoundedRate = Run(argv[1], false, std::string(argv[2]) + "-bounded-rate", false, true);
    const auto StableClock = Run(argv[1], false, "", true, false, true);
    const auto UnstableRate = Run(argv[1], false, "", false, false, false, true);
    if (Control.TimestampNormalizationRejected || !Control.TimestampNormalizationActive ||
        Paused.TimestampNormalizationRejected || !Paused.TimestampNormalizationActive ||
        Control.TimestampPauseResets != 0 || Paused.TimestampPauseResets != 1 ||
        Paused.TimestampLastPauseGapMs < 15000 || Paused.TimestampRetryAttempts != 0 ||
        Control.DroppedVideoPackets != 0 || Paused.DroppedVideoPackets != 0 ||
        Control.AcceptedVideoPackets != 2799 || Paused.AcceptedVideoPackets != 2798)
        return 1;
    if (Mismatched.TimestampNormalizationRejected || !Mismatched.TimestampNormalizationActive ||
        Mismatched.DroppedVideoPackets != 0 || Mismatched.TimestampArrivalRateAdjustments == 0)
        return 1; // wrong nominal FPS does not make a proven faulty source clock trustworthy
    if (!UnstableRate.TimestampNormalizationRejected)
        return 1; // inconsistent delivery windows are not an estimate of frame cadence
    if (BoundedRate.TimestampNormalizationRejected || !BoundedRate.TimestampNormalizationActive ||
        BoundedRate.DroppedVideoPackets != 0 || BoundedRate.AcceptedVideoPackets != 2799 ||
        BoundedRate.TimestampArrivalRateAdjustments == 0 || BoundedRate.TimestampArrivalDurationMs < 55.5 ||
        BoundedRate.TimestampArrivalDurationMs > 56.5 || std::abs(BoundedRate.VideoPhaseErrorMs) > 750)
        return 1; // qualified broken DTS + clean bounded delivery rate retains all reference pictures
    if (StableClock.TimestampNormalizationActive || StableClock.TimestampArrivalRateAdjustments != 0 ||
        StableClock.TimestampNormalizationRejected || StableClock.DroppedVideoPackets != 0 ||
        StableClock.AcceptedVideoPackets != 2799)
        return 1; // delivery delay alone must not replace an intact source clock
    std::cout << "PASS: pause/burst earns fresh cadence and resets at a clean keyframe;"
              << " no rejection or timestamp drops, one pending-frame retirement,"
              << " no cadence-retry budget consumed, bounded rate learns without drops, all generations saved\n";
}
