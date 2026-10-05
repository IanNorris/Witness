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
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void SetPacketTraceBeforeIngest(LiveOutputStream& Live, bool Enabled) {
        Live._LiveData->_RetainPacketTrace = Enabled;
    }
    static void SetMuxTraceBeforeIngest(LiveOutputStream& Live, bool Enabled) {
        Live._LiveData->_RetainMuxTrace = Enabled;
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
struct MediaOutput {
    std::vector<std::vector<uint8_t>> Bytes;
    std::vector<std::array<uint64_t, 8>> Events;
    std::vector<double> Durations;
    bool operator==(const MediaOutput&) const = default;
};
static MediaOutput Run(const char* Fixture, bool ClearTrace, bool RemoveFunctionalEvidence = false,
                     bool DisableTrace = false, bool ExpectedEnvironmentTrace = true,
                     bool DisableMuxTrace = false, bool ExpectedEnvironmentMuxTrace = true) {
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
    MediaOutput Emitted;
    LiveOutputStream Live("", &Input, 1);
    Check(Live.GetStreamingDiagnostics(false).PacketTraceEnabled == ExpectedEnvironmentTrace,
          "stream ignored packet-trace environment setting");
    Check(Live.GetStreamingDiagnostics(false).MuxTraceEnabled == ExpectedEnvironmentMuxTrace,
          "stream ignored mux-trace environment setting");
    LiveTimestampRecoveryTestAccess::SetPacketTraceBeforeIngest(Live, !DisableTrace);
    LiveTimestampRecoveryTestAccess::SetMuxTraceBeforeIngest(Live, !DisableMuxTrace);
    Live.SetEventCallback([&](const LiveStreamEvent& Event) {
        if (Event.Data && !Event.Data->empty()) Emitted.Bytes.push_back(*Event.Data);
        Emitted.Events.push_back({static_cast<uint64_t>(Event.EventType),
            static_cast<uint64_t>(Event.SegmentIndex), static_cast<uint64_t>(Event.PartIndex),
            static_cast<uint64_t>(Event.Generation), Event.ByteSize, Event.TransportHash,
            Event.Independent, Event.KeyframeSeekSafe});
        Emitted.Durations.push_back(Event.Duration);
    });
    Live.SetObservedTimestampRegressionRepairAllowed(true);
    std::unique_ptr<AVPacket, FreePacket> Packet(av_packet_alloc()), Duplicate;
    Check(bool(Packet), "packet allocation failed");
    int Frames = 0;
    // Cross actual fixture keyframes so segment retention is exercised too.
    while (Frames < 240 && av_read_frame(Data.FormatContext, Packet.get()) >= 0) {
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
    Check(Frames == 240 && LiveTimestampRecoveryTestAccess::HasNoBFrames(Live), "insufficient no-B-frame input");
    const auto Before = Live.GetStreamingDiagnostics();
    Check(Before.PacketTraceEnabled == !DisableTrace, "incorrect trace coverage flag");
    Check(Before.RecentPackets.empty() == DisableTrace, "trace retention did not match requested mode");
    Check(Before.MuxTraceEnabled == !DisableMuxTrace &&
          Before.RecentSegments.empty() == DisableMuxTrace &&
          Before.RecentFragments.empty() == DisableMuxTrace,
          "mux ring retention did not match requested mode");
    Check(Before.TotalSegments > 0 && Before.InitStructureObserved && Before.InitStructureValid,
          "mux observer setting disabled summary or functional structure checks");
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
    if (DisableMuxTrace) Check(After.RecentFragments.empty() && After.RecentSegments.empty(),
                              "disabled mux trace accumulated retained history");
    Check(After.TotalSegments == Before.TotalSegments &&
          After.TimestampNormalizationActive == Before.TimestampNormalizationActive,
          "rejected duplicate changed segment/normalization summary");
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
    Check(!Emitted.Bytes.empty(), "no emitted media to compare");
    return Emitted;
}
int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) return 2;
    try {
        const bool ExpectedTrace = argc == 2 || std::string(argv[2]) == "--expect-trace-on";
        if (argc >= 3 && !ExpectedTrace && std::string(argv[2]) != "--expect-trace-off") return 2;
        const bool ExpectedMuxTrace = argc < 4 || std::string(argv[3]) == "--expect-mux-on";
        if (argc == 4 && !ExpectedMuxTrace && std::string(argv[3]) != "--expect-mux-off") return 2;
        const auto Retained = Run(argv[1], false, false, false, ExpectedTrace, false, ExpectedMuxTrace);
        const auto Cleared = Run(argv[1], true, false, false, ExpectedTrace, false, ExpectedMuxTrace);
        const auto Disabled = Run(argv[1], false, false, true, ExpectedTrace, false, ExpectedMuxTrace);
        const auto MuxDisabled = Run(argv[1], false, false, false, ExpectedTrace, true, ExpectedMuxTrace);
        const auto BothDisabled = Run(argv[1], false, false, true, ExpectedTrace, true, ExpectedMuxTrace);
        Check(Retained == Cleared && Retained == Disabled && Retained == MuxDisabled && Retained == BothDisabled,
              "optional tracing changed emitted media bytes or event metadata");
        Run(argv[1], true, true, false, ExpectedTrace, false, ExpectedMuxTrace);
        std::cout << "PASS: packet/mux trace on/off preserves duplicate decisions, media bytes and event metadata; negative control verified\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
