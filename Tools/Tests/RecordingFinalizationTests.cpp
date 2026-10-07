#include "InputStream.h"
#include "OutputStream.h"
#include "StreamData.h"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Witness::Camera;
static void Check(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}

int main(int Argc, char** Argv) {
    try {
        Check(Argc == 3, "usage: RecordingFinalizationTests input new-output-prefix");
        const std::filesystem::path Prefix(Argv[2]);
        Check(std::filesystem::is_directory(Prefix.parent_path()), "owned output directory missing");
        for (const bool FailTrailer : {false, true}) {
            const auto Path = Prefix.string() + (FailTrailer ? "-failed.mp4" : "-success.mp4");
            Check(!std::filesystem::exists(Path), "refusing to overwrite media evidence");
            InputStreamSetup Setup;
            Setup.GetTimestamp = []() -> uint64_t { return 1; };
            Setup.PassthroughOnly = true;
            Setup.ExportMotionVectors = false;
            InputStream Input(Setup, 1, nullptr, Argv[1]);
            Check(Input.Initialize() == CameraStreamError::Success, "input failed");
            OutputStream Output(Path, &Input, false, false, false, false);
            Check(Output.Initialize() == CameraStreamError::Success, "output failed");
            AVPacket* Packet = av_packet_alloc();
            Check(Packet != nullptr, "packet allocation failed");
            int Packets = 0;
            while (av_read_frame(Input.GetData().FormatContext, Packet) >= 0) {
                Check(Output.WriteInterleavedPacket(Packet) == CameraStreamError::Success, "packet write failed");
                av_packet_unref(Packet);
                ++Packets;
            }
            av_packet_free(&Packet);
            Check(Packets > 100, "insufficient fixture coverage");
            if (FailTrailer) Output.GetData().FormatContext->pb->error = AVERROR(EIO);
            const auto Result = Output.CloseFile();
            Check((Result == CameraStreamError::Success) != FailTrailer, "wrong finalization outcome");
            Check(Output.GetData().FormatContext->pb == nullptr, "owned file handle not closed");
            Check(Output.CloseFile() == Result, "duplicate close changed original outcome");
            Output.Shutdown();
            Output.Shutdown();
            Check(Output.GetData().FormatContext == nullptr, "mux context not released");
            std::filesystem::rename(Path, Path + ".renamed");
            std::filesystem::rename(Path + ".renamed", Path);
            std::cout << (FailTrailer ? "failed" : "success") << " packets=" << Packets << '\n';
        }
        InputStreamSetup Setup;
        Setup.GetTimestamp = []() -> uint64_t { return 1; };
        Setup.PassthroughOnly = true;
        InputStream Input(Setup, 1, nullptr, Argv[1]);
        Check(Input.Initialize() == CameraStreamError::Success, "initialization fixture failed");
        const auto MissingDirectory = Prefix.string() + "-missing";
        Check(!std::filesystem::exists(MissingDirectory), "missing-directory fixture already exists");
        OutputStream Unopened(MissingDirectory + "/clip.mp4", &Input, false, false, false, false);
        Check(Unopened.Initialize() != CameraStreamError::Success, "invalid path accepted");
        Check(Unopened.CloseFile() != CameraStreamError::Success, "unopened output reported finalized");
        Unopened.Shutdown();
        Check(Unopened.GetData().FormatContext == nullptr, "failed initialization leaked mux context");
        std::cout << "PASS real-file success, injected IO failure, duplicate close, failed initialization\n";
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
