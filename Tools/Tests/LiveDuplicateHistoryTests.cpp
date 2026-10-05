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

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static CameraStreamError Write(LiveOutputStream& Live, const AVPacket* Packet) {
        return Live.WritePacketWithKnownDuration(Packet, Live.BeginDiagnosticActivity());
    }
    static void ClearOptionalTrace(LiveOutputStream& Live) {
        // Do not reset the independently retained functional evidence.
        Live._LiveData->_PacketDiagRingCount = 0;
        Live._LiveData->_PacketDiagRingPos = 0;
    }
    static bool HasWrittenTrace(const LiveOutputStream& Live) {
        return Live._LiveData->_PacketDiagRingCount > 0 && !Live._LiveData->_HasBFrames;
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
static void Run(const char* Fixture, bool ClearTrace, bool RemoveFunctionalEvidence = false) {
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
    LiveOutputStream Live("", &Input, 1);
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
    Check(Frames == 40 && LiveTimestampRecoveryTestAccess::HasWrittenTrace(Live), "no no-B-frame written evidence");
    const auto Before = Live.GetStreamingDiagnostics(false);
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
    const auto After = Live.GetStreamingDiagnostics(false);
    if (RemoveFunctionalEvidence) {
        Check(After.AcceptedVideoPackets == Before.AcceptedVideoPackets + 1 &&
              After.RepairedVideoTimestamps == Before.RepairedVideoTimestamps + 1,
              "negative control did not demonstrate lost duplicate evidence");
        return;
    }
    Check(After.AcceptedVideoPackets == Before.AcceptedVideoPackets, "duplicate was accepted after trace mutation");
    Check(After.DroppedVideoPackets == Before.DroppedVideoPackets + 1 &&
          After.NonMonotonicInputPackets == Before.NonMonotonicInputPackets + 1,
          "duplicate was not rejected as non-monotonic input");
    Check(After.RepairedVideoTimestamps == Before.RepairedVideoTimestamps, "duplicate was mistaken for a distinct timestamp-repair candidate");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        Run(argv[1], false);
        Run(argv[1], true);
        Run(argv[1], true, true);
        std::cout << "PASS: production duplicate decisions are independent of optional packet trace retention\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
