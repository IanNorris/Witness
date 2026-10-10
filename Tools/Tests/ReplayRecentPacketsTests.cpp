#include <InputStream.h>
#include <OutputStream.h>
#include <ContinuousOutputStream.h>
#include "StreamData.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Witness::Camera;
uint64_t Clock = 1000;
void Check(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
int main(int argc, char** argv) {
    try {
        Check(argc == 3 && !std::filesystem::exists(argv[2]), "Usage: ReplayRecentPacketsTests synthetic-input new-directory");
        const auto directory = std::filesystem::absolute(argv[2]); std::filesystem::create_directories(directory);
        InputStreamSetup setup; setup.GetTimestamp = []() -> uint64_t {return Clock;};
        setup.PassthroughOnly = true; setup.ExportMotionVectors = false;
        InputStream input(setup, 1, nullptr, argv[1]); Check(input.Initialize() == CameraStreamError::Success, "Input init failed");
        OutputStream normal((directory / "normal.mp4").string(), &input, false, false, false, false);
        Check(normal.Initialize() == CameraStreamError::Success, "Normal recording init failed");
        ContinuousOutputStream spool((directory / "spool").string(), 1, &input);
        std::string spoolPath;
        spool.SetSegmentCompleteCallback([&](int, int64_t start, int64_t, int, const std::string& path) {
            Check(start == 1000, "History wall-clock origin was replaced by current time"); spoolPath = path;
        });
        double observed = 0;
        bool replayed = false, closed = false;
        input.SetPacketCallback([&](const AVPacket* packet) {
            if (packet->stream_index == input.GetData().ChosenStreamIndex && packet->pts != AV_NOPTS_VALUE) {
                observed = packet->pts * av_q2d(input.GetData().FormatContext->streams[packet->stream_index]->time_base);
                Clock = 1000 + static_cast<uint64_t>(observed);
            }
            if (replayed && !closed) {
                if (observed >= 7) {spool.Finalize(); closed = true;}
                else Check(spool.WritePacket(packet) == CameraStreamError::Success, "Live spool packet failed");
            }
        });
        while (input.ProcessFrame(nullptr, &normal, nullptr) == CameraStreamError::Success) {
            if (replayed || observed < 3) continue;
            const auto before = input.GetData().KeyframeStates;
            int audio = 0, video = 0, nonzero = 0;
            const auto copied = input.ReplayRecentPackets([&](const AVPacket* packet, int64_t timestamp) {
                if (!video && packet->stream_index == input.GetData().ChosenStreamIndex) {
                    Check((packet->flags & AV_PKT_FLAG_KEY) != 0, "History did not begin at a RAP");
                    spool.SetInitialSegmentTimestamp(timestamp);
                }
                if (packet->stream_index == input.GetData().ChosenStreamIndex) {++video; if (packet->pts != 0) ++nonzero;}
                else if (packet->stream_index == input.GetData().ChosenAudioStreamIndex) ++audio;
                Check(spool.WritePacket(packet) == CameraStreamError::Success, "History replay write failed");
            });
            Check(copied > 0 && video > 1 && nonzero > 0 && audio > 0, "History lost video timing or audio");
            Check(before.size() == input.GetData().KeyframeStates.size(), "Replay consumed normal recording history");
            for (size_t i = 0; i < before.size(); ++i)
                Check(before[i].StreamIndex == input.GetData().KeyframeStates[i].StreamIndex
                    && before[i].PacketCount == input.GetData().KeyframeStates[i].PacketCount, "Replay changed normal recording markers");
            replayed = true;
        }
        spool.Finalize(); Check(replayed && closed && !spoolPath.empty(), "Independent spool never completed");
        Check(normal.CloseFile() == CameraStreamError::Success && normal.GetClipLength() > 7.5,
            "Independent capture interrupted the normal recording");
        std::cout << "PASS retained RAP/audio clocks, untouched normal history, independent live continuation and stop\n";
        return 0;
    } catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
}
