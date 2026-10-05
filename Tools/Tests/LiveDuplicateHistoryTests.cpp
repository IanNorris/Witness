// Offline production-mux regression. Supply a video-only no-B-frame fixture.
// Clearing optional trace storage must not change a duplicate repair decision.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include "LiveOutputStreamData.h"
#include "StreamData.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void SetPacketTraceBeforeIngest(LiveOutputStream& Live, bool Enabled) {
        Live._LiveData->_RetainPacketTrace = Enabled;
    }
    static CameraStreamError Write(LiveOutputStream& Live, const AVPacket* Packet) {
        return Live.WritePacketWithKnownDuration(Packet, Live.BeginDiagnosticActivity());
    }
    static void ClearOptionalTrace(LiveOutputStream& Live) {
        // Do not reset the independently retained functional evidence.
        Live._LiveData->_PacketDiagRingCount = 0;
        Live._LiveData->_PacketDiagRingPos = 0;
    }
    static bool HasNoBFrames(const LiveOutputStream& Live) {
        return !Live._LiveData->_HasBFrames;
    }
    static void ClearFunctionalEvidenceForNegativeControl(LiveOutputStream& Live) {
        Live._LiveData->_RecentWrittenVideoPackets = {};
    }
};
}
struct FreePacket {
    void operator()(AVPacket* Packet) const { av_packet_free(&Packet); }
};
static void Check(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}
using MediaBytes = std::vector<std::vector<uint8_t>>;
static MediaBytes Run(const char* Fixture, bool ClearTrace, bool RemoveFunctionalEvidence = false,
                     bool DisableTrace = false, bool ExpectedEnvironmentTrace = true) {
    InputStreamSetup Setup;
    Setup.GetTimestamp = []() -> uint64_t { return 1; };
    Setup.PassthroughOnly = true;
    Setup.ExportMotionVectors = false;
    Setup.HistoricalPacketBufferSeconds = 0;
    InputStream Input(Setup, 8, nullptr, Fixture);
    Check(Input.Initialize() == CameraStreamError::Success, "fixture initialization failed");
    auto& Data = Input.GetData();
    Check(Data.FormatContext->nb_streams == 1, "fixture must be video-only");
    Data.FormatContext->streams[Data.ChosenStreamIndex]->time_base = {1, 90000};
    MediaBytes Emitted;
    LiveOutputStream Live("", &Input, 1);
    Check(Live.GetStreamingDiagnostics(false).PacketTraceEnabled == ExpectedEnvironmentTrace,
          "stream ignored packet-trace environment setting");
    LiveTimestampRecoveryTestAccess::SetPacketTraceBeforeIngest(Live, !DisableTrace);
    Live.SetEventCallback([&](const LiveStreamEvent& Event) {
        if (Event.Data && !Event.Data->empty()) Emitted.push_back(*Event.Data);
    });
    Live.SetObservedTimestampRegressionRepairAllowed(true);
    std::unique_ptr<AVPacket, FreePacket> Packet(av_packet_alloc()), Duplicate;
    Check(bool(Packet), "packet allocation failed");
    int Frames = 0;
    while (Frames < 40 && av_read_frame(Data.FormatContext, Packet.get()) >= 0) {
        if (Packet->stream_index == Data.ChosenStreamIndex) {
            Packet->dts = Packet->pts = Frames * 4500LL;
            Packet->duration = 4500;
            Check(LiveTimestampRecoveryTestAccess::Write(Live, Packet.get()) == CameraStreamError::Success,
                  "production mux write failed");
            Duplicate.reset(av_packet_clone(Packet.get()));
            Check(bool(Duplicate), "duplicate allocation failed");
            ++Frames;
        }
        av_packet_unref(Packet.get());
    }
    Check(Frames == 40 && LiveTimestampRecoveryTestAccess::HasNoBFrames(Live), "insufficient no-B-frame input");
    const auto Before = Live.GetStreamingDiagnostics();
    Check(Before.PacketTraceEnabled == !DisableTrace, "incorrect trace coverage flag");
    Check(Before.RecentPackets.empty() == DisableTrace, "trace retention did not match requested mode");
    Check(Before.AcceptedVideoPackets >= 30, "insufficient established monotonic packets");
    // Equal DTS is rejected independently of payload history. Use a bounded
    // regression instead, which would otherwise qualify for repair after the
    // monotonic warmup; only the repeated payload should disqualify it.
    Duplicate->dts -= 4500;
    Duplicate->pts = Duplicate->dts;
    if (ClearTrace) LiveTimestampRecoveryTestAccess::ClearOptionalTrace(Live);
    if (RemoveFunctionalEvidence)
        LiveTimestampRecoveryTestAccess::ClearFunctionalEvidenceForNegativeControl(Live);
    Check(LiveTimestampRecoveryTestAccess::Write(Live, Duplicate.get()) == CameraStreamError::Success,
          "duplicate decision failed");
    const auto After = Live.GetStreamingDiagnostics();
    if (DisableTrace) Check(After.RecentPackets.empty(), "disabled trace accumulated packet details");
    if (RemoveFunctionalEvidence) {
        Check(After.AcceptedVideoPackets == Before.AcceptedVideoPackets + 1 &&
              After.RepairedVideoTimestamps == Before.RepairedVideoTimestamps + 1,
              "negative control did not demonstrate lost duplicate evidence");
        return Emitted;
    }
    Check(After.AcceptedVideoPackets == Before.AcceptedVideoPackets, "duplicate was accepted after trace mutation");
    Check(After.DroppedVideoPackets == Before.DroppedVideoPackets + 1 &&
          After.NonMonotonicInputPackets == Before.NonMonotonicInputPackets + 1,
          "duplicate was not rejected as non-monotonic input");
    Check(After.RepairedVideoTimestamps == Before.RepairedVideoTimestamps, "duplicate was mistaken for a distinct timestamp-repair candidate");
    Check(!Emitted.empty(), "no emitted media to compare");
    return Emitted;
}
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    try {
        const bool ExpectedTrace = argc == 2 || std::string(argv[2]) == "--expect-trace-on";
        if (argc == 3 && !ExpectedTrace && std::string(argv[2]) != "--expect-trace-off") return 2;
        const auto Retained = Run(argv[1], false, false, false, ExpectedTrace);
        const auto Cleared = Run(argv[1], true, false, false, ExpectedTrace);
        const auto Disabled = Run(argv[1], false, false, true, ExpectedTrace);
        Check(Retained == Cleared && Retained == Disabled,
              "optional packet tracing changed emitted init/fragment/segment bytes");
        Run(argv[1], true, true, false, ExpectedTrace);
        std::cout << "PASS: trace on/off preserves duplicate decisions and emitted media bytes; negative control verified\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
