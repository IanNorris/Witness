// Offline actual-mux/log-callback regression; no server, live DB or cameras.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include "LiveOutputStreamData.h"
#include "StreamData.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void Mode(LiveOutputStream& Live, bool Retain) { Live._LiveData->_RetainMediaTrace = Retain; }
    static CameraStreamError Write(LiveOutputStream& Live, const AVPacket* Packet) {
        return Live.WritePacketWithKnownDuration(Packet, Live.BeginDiagnosticActivity());
    }
    static void ClearHistory(LiveOutputStream& Live) {
        Live._LiveData->_MediaEventRingCount = 0;
        Live._LiveData->_MediaEventRingPos = 0;
    }
};
}
static void Check(bool Value, const char* Message) { if (!Value) throw std::runtime_error(Message); }
struct FreePacket { void operator()(AVPacket* Packet) const { av_packet_free(&Packet); } };
struct Output {
    std::vector<std::vector<uint8_t>> Bytes;
    std::vector<std::array<uint64_t, 9>> Events;
    std::vector<double> Durations;
    bool operator==(const Output&) const = default;
};
static void ClusterLimits() {
    FFmpegLogClusters Clusters;
    Check(Clusters.Record(true, "decode", "h264", "failure #", 0, 0) == 1, "first cluster");
    Check(Clusters.Record(true, "decode", "h264", "failure #", 0, 60000) == 2, "gap boundary");
    Check(Clusters.Record(true, "decode", "h264", "failure #", 0, 120001) == 1, "gap expiry");
    Check(Clusters.Record(true, "decode", "h264", "failure #", 1, 120002) == 1, "generation isolation");
    Check(Clusters.Record(false, "decode", "h264", "failure #", 1, 120002) == 1, "severity isolation");
    Check(Clusters.Record(true, "demux", "h264", "failure #", 1, 120002) == 1, "phase isolation");
    Check(Clusters.Record(true, "decode", "hevc", "failure #", 1, 120002) == 1, "component isolation");
    FFmpegLogClusters Age;
    for (int Step = 0; Step <= 5; ++Step)
        Check(Age.Record(true, "decode", "h264", "failure", 0, Step * 60000) == Step + 1,
              "cluster age boundary");
    Check(Age.Record(true, "decode", "h264", "failure", 0, 300001) == 1, "cluster age expiry");
    Check(Age.Record(true, "decode", "h264", "failure", 0, -1) == 1, "backward clock");
    FFmpegLogClusters Bounded;
    for (size_t Index = 0; Index < FFmpegLogClusters::Capacity + 3; ++Index)
        Check(Bounded.Record(true, "decode", "h264", std::to_string(Index), 0, 0) == 1, "new cluster");
    Check(Bounded.Size() == FFmpegLogClusters::Capacity, "unbounded cluster storage");
    Check(Bounded.Record(true, "decode", "h264", "0", 0, 1) == 1, "old cluster not evicted");
}
static Output Run(const char* Fixture, bool Retain, bool ExpectedEnvironment) {
    InputStreamSetup Setup;
    Setup.GetTimestamp = []() -> uint64_t { return 1; };
    Setup.PassthroughOnly = true;
    Setup.ExportMotionVectors = false;
    Setup.HistoricalPacketBufferSeconds = 0;
    InputStream Input(Setup, 8, nullptr, Fixture);
    Check(Input.Initialize() == CameraStreamError::Success, "fixture initialization");
    auto& Data = Input.GetData();
    Check(Data.FormatContext->nb_streams == 1, "fixture must be video only");
    Data.FormatContext->streams[Data.ChosenStreamIndex]->time_base = {1, 90000};
    Output Emitted;
    LiveOutputStream Live("", &Input, 1);
    Check(Live.GetStreamingDiagnostics(false).MediaTraceEnabled == ExpectedEnvironment, "environment coverage");
    LiveTimestampRecoveryTestAccess::Mode(Live, Retain);
    Live.SetEventCallback([&](const LiveStreamEvent& Event) {
        if (Event.Data && !Event.Data->empty()) Emitted.Bytes.push_back(*Event.Data);
        Emitted.Events.push_back({static_cast<uint64_t>(Event.EventType),
            static_cast<uint64_t>(Event.SegmentIndex), static_cast<uint64_t>(Event.PartIndex),
            static_cast<uint64_t>(Event.Generation), Event.ByteSize, Event.TransportHash,
            Event.Independent, Event.KeyframeSeekSafe, static_cast<uint64_t>(Event.DecodeErrorFlags)});
        Emitted.Durations.push_back(Event.Duration);
    });
    std::unique_ptr<AVPacket, FreePacket> Packet(av_packet_alloc());
    Check(bool(Packet), "packet allocation");
    int Frames = 0;
    while (Frames < 240 && av_read_frame(Data.FormatContext, Packet.get()) >= 0) {
        if (Packet->stream_index == Data.ChosenStreamIndex) {
            Packet->dts = Packet->pts = Frames * 4500LL;
            Packet->duration = 4500;
            if (Frames == 10) {
                const auto Before = GetFFmpegThreadLogCounts();
                FFmpegLogContextScope Scope(8, "fixture-decode", &Live, Live.BeginDiagnosticActivity());
                av_log(nullptr, AV_LOG_ERROR, "synthetic slice failure 1\n");
                av_log(nullptr, AV_LOG_ERROR, "synthetic slice failure 2\n");
                av_log(nullptr, AV_LOG_WARNING, "Packet duration: stream 1\n");
                av_log(nullptr, AV_LOG_WARNING, "pts has no value\n");
                const auto After = GetFFmpegThreadLogCounts();
                Check(Scope.HasError() && After.Errors == Before.Errors + 2 &&
                      After.AudioDurationWarnings == Before.AudioDurationWarnings + 1 &&
                      After.MissingPtsWarnings == Before.MissingPtsWarnings + 1,
                      "media retention disabled functional FFmpeg error detection/counts");
                Live.NotifyDecodeCorruption(0);
                Live.NotifyDecodeCorruption(0); // still one hold transition
            }
            Check(LiveTimestampRecoveryTestAccess::Write(Live, Packet.get()) == CameraStreamError::Success,
                  "actual mux write");
            ++Frames;
        }
        av_packet_unref(Packet.get());
    }
    Check(Frames == 240, "insufficient fixture packets");
    const auto Stats = Live.GetStreamingDiagnostics();
    Check(Stats.MediaTraceEnabled == Retain && Stats.RecentMediaEvents.empty() != Retain,
          "media history retention mismatch");
    if (Retain) {
        bool Found = false;
        for (const auto& Event : Stats.RecentMediaEvents) {
            if (Event.Category != "ffmpeg" || Event.Phase != "fixture-decode" || Event.Severity != "error") continue;
            Check(Event.Count == 2 && Event.ActivityID > 0 && Event.Sequence > 0 &&
                  Event.Message == "synthetic slice failure 1" && Event.LastMessage == "synthetic slice failure 2",
                  "retained FFmpeg feed lost first/latest occurrence or activity correlation");
            Found = true;
        }
        Check(Found, "missing retained FFmpeg error cluster");
    }
    Check(Stats.DecodeCorruptionEvents == 1 && Stats.DecodeRecoveryEvents == 1 &&
          Stats.AcceptedVideoPackets == 240 && Stats.DroppedVideoPackets == 0,
          "media retention changed corruption/recovery or packet counters");
    size_t Holds = 0, Recoveries = 0;
    for (size_t Index = 0; Index < Emitted.Events.size(); ++Index) {
        const auto& Event = Emitted.Events[Index];
        if (Event[0] == LiveStreamEvent::DecodeCorruption) ++Holds;
        if (Event[0] != LiveStreamEvent::DecodeRecovery) continue;
        ++Recoveries;
        Check(Index + 1 < Emitted.Events.size(), "missing recovery partial");
        const auto& Next = Emitted.Events[Index + 1];
        Check(Next[0] == LiveStreamEvent::PartialReady && Next[6] &&
              Next[1] == Event[1] && Next[2] == Event[2] && Next[3] == Event[3],
              "recovery was not ordered immediately before matching independent partial");
    }
    Check(Holds == 1 && Recoveries == 1 && !Emitted.Bytes.empty(), "missing player control transitions");
    // Passive event eviction must never reset the independent log limiter.
    for (uint64_t Count = 1; Count <= 120; ++Count)
        Check(Live.RecordFFmpegLog(AV_LOG_ERROR, "repeat-test", "h264", "failure 123", 0) == Count,
              "incorrect repeat count");
    LiveTimestampRecoveryTestAccess::ClearHistory(Live);
    Check(Live.RecordFFmpegLog(AV_LOG_ERROR, "repeat-test", "h264", "failure 456", 0) == 121,
          "optional history controls raw log suppression");
    Live.SetEventCallback({});
    return Emitted;
}
int main(int Argc, char** Argv) {
    if (Argc != 3) return 2;
    try {
        ClusterLimits();
        const bool Expected = std::string(Argv[2]) == "1";
        Check(Run(Argv[1], true, Expected) == Run(Argv[1], false, Expected),
              "media trace changed bytes, player controls or transport metadata");
        std::cout << "PASS: bounded log clusters; retained/disabled media history preserves FFmpeg counts, hold/recovery controls and exact media\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
