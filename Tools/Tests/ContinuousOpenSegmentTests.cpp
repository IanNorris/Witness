#include "ContinuousOutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Witness::Camera;
static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        Check(argc == 3, "usage: ContinuousOpenSegmentTests synthetic-input new-directory");
        const std::filesystem::path directory(argv[2]);
        Check(!std::filesystem::exists(directory), "refusing to overwrite test output");
        std::filesystem::create_directories(directory);
        InputStreamSetup setup;
        setup.GetTimestamp = []() -> uint64_t { return 1; };
        setup.PassthroughOnly = true;
        setup.ExportMotionVectors = false;
        InputStream input(setup, 1, nullptr, argv[1]);
        Check(input.Initialize() == CameraStreamError::Success, "input failed");
        ContinuousOutputStream output(directory.string(), 1, &input);
        output.SetFragmentedRecordingEnabled(true);
        int completed = 0;
        output.SetSegmentCompleteCallback([&](int, int64_t, int64_t, int, const std::string&) { ++completed; });
        AVPacket* packet = av_packet_alloc();
        Check(packet != nullptr, "allocation failed");
        int prefixes = 0;
        int64_t previous = 0;
        double previousDuration = 0;
        while (av_read_frame(input.GetData().FormatContext, packet) >= 0) {
            Check(output.WritePacket(packet) == CameraStreamError::Success, "write failed");
            av_packet_unref(packet);
            const auto snapshot = output.GetCommittedSegment();
            if (snapshot.CommittedBytes == previous) continue;
            Check(completed == 0, "segment unexpectedly finalized before replay");
            Check(snapshot.CommittedBytes > previous && snapshot.Duration >= previousDuration, "non-monotonic committed prefix");
            Check(snapshot.CommittedBytes <= static_cast<int64_t>(std::filesystem::file_size(snapshot.FilePath)), "unflushed bytes published");
            const auto prefix = directory / ("prefix-" + std::to_string(++prefixes) + ".mp4");
            std::ifstream source(snapshot.FilePath, std::ios::binary);
            std::ofstream copy(prefix, std::ios::binary);
            char buffer[65536];
            for (int64_t left = snapshot.CommittedBytes; left > 0;) {
                const auto count = std::min<int64_t>(left, sizeof(buffer));
                source.read(buffer, count);
                Check(source.gcount() == count, "prefix read incomplete");
                copy.write(buffer, count);
                left -= count;
            }
            copy.close();
            // Demux the committed bytes while the original writer remains open.
            AVFormatContext* reader = nullptr;
            Check(avformat_open_input(&reader, prefix.string().c_str(), nullptr, nullptr) >= 0, "open-prefix playback failed");
            Check(avformat_find_stream_info(reader, nullptr) >= 0, "open-prefix metadata failed");
            AVPacket* readPacket = av_packet_alloc();
            int packets = 0;
            while (av_read_frame(reader, readPacket) >= 0) { ++packets; av_packet_unref(readPacket); }
            av_packet_free(&readPacket);
            avformat_close_input(&reader);
            Check(packets > 1, "no playable prefix packets");
            std::cout << "prefix=" << prefixes << " bytes=" << snapshot.CommittedBytes << " duration=" << snapshot.Duration << " packets=" << packets << '\n';
            previous = snapshot.CommittedBytes;
            previousDuration = snapshot.Duration;
        }
        av_packet_free(&packet);
        Check(prefixes >= 3, "insufficient GOP coverage");
        output.Finalize();
        Check(completed == 1 && output.GetCommittedSegment().CommittedBytes == 0, "finalization did not retire active prefix");
        output.Finalize();
        Check(completed == 1, "duplicate finalization callback");
        std::cout << "PASS committed prefixes readable before original file closes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
