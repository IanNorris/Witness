#include "InputStream.h"
#include "LiveOutputStream.h"
#include "LiveOutputStreamData.h"
#include "StreamData.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static CameraStreamError Write(LiveOutputStream& Live, const AVPacket* Packet) {
        return Live.WritePacketWithKnownDuration(Packet, Live.BeginDiagnosticActivity());
    }
    static bool Flush(LiveOutputStream& Live) { return Live.FlushPartialSegment(false); }
};
}
static void Check(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}

int main(int Argc, char** Argv) {
    try {
        Check(Argc == 4, "usage: LiveAacRemuxTests input output adts|raw");
        const bool ExpectAdts = std::string(Argv[3]) == "adts";
        InputStreamSetup Setup;
        Setup.GetTimestamp = []() -> uint64_t { return 1; };
        Setup.PassthroughOnly = true;
        Setup.ExportMotionVectors = false;
        Setup.HistoricalPacketBufferSeconds = 0;
        InputStream Input(Setup, 8, nullptr, Argv[1]);
        Check(Input.Initialize() == CameraStreamError::Success, "input failed");
        auto& Data = Input.GetData();
        Check(Data.HasAudio, "AAC fixture required");
        const auto* Audio = Data.FormatContext->streams[Data.ChosenAudioStreamIndex];
        Check(ExpectAdts || Audio->codecpar->extradata_size > 0, "raw AAC configuration missing");
        AacMp4BitstreamFilter Filter;
        Check(Filter.Initialize(Audio->codecpar, Audio->time_base) >= 0, "filter initialization failed");
        std::ofstream Output(Argv[2], std::ios::binary);
        Check(bool(Output), "output open failed");
        LiveOutputStream Live("", &Input, 1);
        int Partials = 0;
        Live.SetEventCallback([&](const LiveStreamEvent& Event) {
            if (Event.EventType == LiveStreamEvent::InitSegmentReady ||
                Event.EventType == LiveStreamEvent::PartialReady) {
                Check(Event.Data && !Event.Data->empty(), "empty emitted media");
                Output.write(reinterpret_cast<const char*>(Event.Data->data()), Event.Data->size());
                if (Event.EventType == LiveStreamEvent::PartialReady) ++Partials;
            }
        });
        AVPacket* Packet = av_packet_alloc();
        Check(Packet != nullptr, "packet allocation failed");
        int AudioPackets = 0, VideoPackets = 0;
        while (av_read_frame(Data.FormatContext, Packet) >= 0) {
            if (Packet->stream_index == Data.ChosenAudioStreamIndex) {
                Check(Packet->size >= 2, "short AAC fixture packet");
                const bool Adts = Packet->data[0] == 0xff && (Packet->data[1] & 0xf0) == 0xf0;
                Check(Adts == ExpectAdts, "fixture framing does not match test");
                AVPacket* Converted = av_packet_clone(Packet);
                Check(Converted != nullptr, "clone failed");
                Check(Filter.Convert(Converted) >= 0, "AAC conversion failed");
                Check(Converted->pts == Packet->pts && Converted->dts == Packet->dts &&
                    Converted->duration == Packet->duration && Converted->flags == Packet->flags &&
                    Converted->stream_index == Packet->stream_index, "conversion changed timing/metadata");
                const int HeaderSize = Adts ? 7 + ((Packet->data[1] & 1) ? 0 : 2) : 0;
                Check(Converted->size == Packet->size - HeaderSize &&
                    std::memcmp(Converted->data, Packet->data + HeaderSize, Converted->size) == 0,
                    "AAC payload changed beyond ADTS header removal");
                av_packet_free(&Converted);
                ++AudioPackets;
            } else if (Packet->stream_index == Data.ChosenStreamIndex) ++VideoPackets;
            const auto OriginalSize = Packet->size;
            const auto OriginalDts = Packet->dts;
            const std::vector<uint8_t> Original(Packet->data, Packet->data + Packet->size);
            Check(LiveTimestampRecoveryTestAccess::Write(Live, Packet) == CameraStreamError::Success,
                "production mux failed");
            Check(Packet->size == OriginalSize && Packet->dts == OriginalDts &&
                (!Packet->size || std::memcmp(Packet->data, Original.data(), Packet->size) == 0),
                "mux mutated input packet");
            av_packet_unref(Packet);
        }
        av_packet_free(&Packet);
        Check(LiveTimestampRecoveryTestAccess::Flush(Live), "final fragment flush failed");
        Output.close();
        Check(bool(Output) && AudioPackets > 100 && VideoPackets > 100 && Partials > 5,
            "insufficient media coverage");
        Check(Live.GetStreamingDiagnostics().InitStructureValid, "invalid fMP4 init");
        Filter.Reset();
        Check(Filter.Initialize(Audio->codecpar, Audio->time_base) >= 0, "filter reset/reinit failed");
        AVPacket* Malformed = av_packet_alloc();
        Check(Malformed && av_new_packet(Malformed, 3) >= 0, "malformed packet allocation failed");
        Malformed->data[0] = 0xff;
        Malformed->data[1] = 0xf1;
        Malformed->data[2] = 0x50;
        Check(Filter.Convert(Malformed) == AVERROR_INVALIDDATA, "malformed ADTS was accepted");
        av_packet_free(&Malformed);
        // Exercise actual reconnect teardown before rebuilding the output.
        Live.SetEventCallback({});
        Live.ResetForReconnect(&Input);
        Check(!Live.GetInitSnapshot().AudioCodec.empty(), "reconnect discarded retained init prematurely");
        std::cout << "PASS audio=" << AudioPackets << " video=" << VideoPackets
                  << " partials=" << Partials << '\n';
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
