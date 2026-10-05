// Offline consumer of the real Camera DLL; no internal state access.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include <chrono>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace Witness::Camera;
static_assert(!std::is_copy_constructible_v<LiveOutputStream>);
static_assert(!std::is_move_constructible_v<LiveOutputStream>);

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static uint64_t Hash(const SegmentBuffer& buffer)
{
    uint64_t result = 14695981039346656037ULL;
    if (buffer) for (uint8_t byte : *buffer) { result ^= byte; result *= 1099511628211ULL; }
    return result;
}

static void TestPreheaderEof(const char* path)
{
    for (int mode = 0; mode < 4; ++mode)
    {
        bool unwound = false;
        try
        {
            InputStreamSetup setup;
            setup.GetTimestamp = []() -> uint64_t {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            };
            setup.PassthroughOnly = true;
            setup.ExportMotionVectors = false;
            setup.HistoricalPacketBufferSeconds = 0;
            InputStream input(setup, 8, nullptr, path);
            Require(input.Initialize() == CameraStreamError::Success, "short fixture initialization");
            LiveOutputStream live("", &input, 1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            CameraStreamError result = CameraStreamError::Success;
            int packets = 0;
            do
            {
                Require(++packets <= 5000 && std::chrono::steady_clock::now() < deadline,
                    "short fixture did not reach EOF within bounds");
                result = input.ProcessFrame(nullptr, nullptr, &live);
            } while (result == CameraStreamError::Success);
            Require(result == CameraStreamError::EndOfFile, "short fixture expected EOF");
            Require(!live.GetInitSegment(), "short fixture unexpectedly wrote a header");
            std::cout << "preheader EOF mode=" << mode << " packets=" << packets << std::endl;
            if (mode == 0) { live.Shutdown(); live.Shutdown(); }
            if (mode == 2) throw mode;
            if (mode == 3) { live.ResetForReconnect(&input); live.Shutdown(); }
            // Mode 1 exercises ordinary destructor-only shutdown.
        }
        catch (int modeThrown)
        {
            Require(mode == 2 && modeThrown == mode, "unexpected unwind sentinel");
            unwound = true;
        }
        Require(unwound == (mode == 2), "exception unwind teardown missing");
    }
    std::cout << "4 cross-DLL preheader EOF teardown paths passed\n";
}

int main(int argc, char** argv)
{
    if (argc != 2 && (argc != 3 || (std::string(argv[2]) != "audio" && std::string(argv[2]) != "preheader"))) return 2;
    const bool expectAudio = argc == 3 && std::string(argv[2]) == "audio";
    try
    {
        if (argc == 3 && std::string(argv[2]) == "preheader")
        {
            TestPreheaderEof(argv[1]);
            return 0;
        }
        {
            LiveOutputStream idle("", nullptr, 1);
            Require(!idle.GetInitSegment(), "idle init");
            idle.Shutdown();
            idle.Shutdown();
        }
        for (int cycle = 0; cycle < 8; ++cycle)
        {
            std::vector<SegmentBuffer> retained;
            std::vector<uint64_t> hashes;
            LiveStreamInitSnapshot retainedInit;
            uint64_t retainedInitHash = 0;
            std::vector<LiveStreamSegment> retainedSegments;
            std::vector<SegmentBuffer> segmentBuffers;
            std::vector<uint64_t> segmentHashes;
            int discontinuities = 0;
            {
                InputStreamSetup setup;
                setup.GetTimestamp = []() -> uint64_t {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                };
                setup.PassthroughOnly = true;
                setup.ExportMotionVectors = false;
                setup.HistoricalPacketBufferSeconds = 0;
                InputStream input(setup, 8, nullptr, argv[1]);
                Require(input.Initialize() == CameraStreamError::Success, "fixture initialization");
                LiveOutputStream live("", &input, 1);
                live.SetPartialTargetDuration(0.15);
                Require(live.GetPartialTargetDuration() == 0.15, "partial target");
                live.SetEventCallback([&](const LiveStreamEvent& event) {
                    if (event.EventType == LiveStreamEvent::Discontinuity) ++discontinuities;
                    if (event.Data) { retained.push_back(event.Data); hashes.push_back(Hash(event.Data)); }
                });
                for (int generation = 0; generation < 2; ++generation)
                {
                    // Audio packets and GOP length make a fixed packet count
                    // insufficient: the backlog includes an unfinished entry.
                    // Keep the original minimum work, but require actual
                    // completed media before testing its retained ownership.
                    std::vector<LiveStreamSegment> currentSegments;
                    const auto Complete = [](const auto& segment) {
                        return segment.Ready && segment.Data && !segment.Data->empty();
                    };
                    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    int packets = 0;
                    do
                    {
                        Require(packets < 5000 && std::chrono::steady_clock::now() < Deadline,
                            "fixture did not produce a completed segment within bounds");
                        Require(input.ProcessFrame(nullptr, nullptr, &live) == CameraStreamError::Success,
                            "packet processing before completed segment");
                        ++packets;
                        live.GetSegments(currentSegments);
                    } while (packets < 100 || !std::any_of(currentSegments.begin(), currentSegments.end(), Complete));
                    const auto snapshot = live.GetInitSnapshot();
                    Require(snapshot.Data && snapshot.Generation == generation, "init generation");
                    if (expectAudio) Require(!snapshot.AudioCodec.empty(), "init audio codec");
                    Require(live.GetInitGeneration() == generation, "generation getter");
                    Require(Hash(live.GetInitSegment()) == Hash(snapshot.Data), "atomic init snapshot");
                    std::cout << "cycle=" << cycle << " generation=" << generation
                        << " packets=" << packets << " complete="
                        << std::count_if(currentSegments.begin(), currentSegments.end(), Complete) << '\n';
                    for (const auto& segment : currentSegments) if (Complete(segment))
                    {
                        retainedSegments.push_back(segment);
                        segmentBuffers.push_back(segment.Data);
                        segmentHashes.push_back(Hash(segment.Data));
                        for (const auto& partial : segment.Partials)
                        {
                            Require(partial.Data && !partial.Data->empty(), "completed segment partial missing media");
                            segmentBuffers.push_back(partial.Data);
                            segmentHashes.push_back(Hash(partial.Data));
                        }
                    }
                    if (generation == 0)
                    {
                        retainedInit = snapshot;
                        retainedInitHash = Hash(snapshot.Data);
                        live.ResetForReconnect(&input);
                    }
                }
                Require(discontinuities == 1, "reconnect notification");
                const auto diagnostics = live.GetStreamingDiagnostics();
                Require(diagnostics.ReconnectCount == 1, "diagnostic reconnect count");
                if (expectAudio)
                {
                    Require(!diagnostics.AudioCodec.empty(), "audio diagnostics");
                    Require(std::any_of(diagnostics.RecentPackets.begin(), diagnostics.RecentPackets.end(),
                        [](const auto& packet) { return packet.Audio && packet.HasOutputDts; }), "muxed audio packet evidence");
                }
                if (cycle % 2 == 0) { live.Shutdown(); live.Shutdown(); }
                // The callback captures only consumer-owned containers which
                // outlive the stream, including its destructor flush.
            }
            Require(retainedInit.Data && !retained.empty(), "retained media");
            Require(retained.size() == hashes.size(), "callback snapshot count");
            for (size_t index = 0; index < retained.size(); ++index)
                Require(Hash(retained[index]) == hashes[index], "published buffer changed or lost after destruction");
            Require(!retainedSegments.empty() && retainedSegments.front().Data, "retained segment ownership");
            Require(Hash(retainedInit.Data) == retainedInitHash, "retained init changed after reconnect/destruction");
            Require(!segmentBuffers.empty() && segmentBuffers.size() == segmentHashes.size(), "completed media snapshots missing");
            for (size_t index = 0; index < segmentBuffers.size(); ++index)
                Require(Hash(segmentBuffers[index]) == segmentHashes[index], "completed segment/partial changed after destruction");
        }
        std::cout << "8 cross-DLL media ownership/reconnect/shutdown cycles passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
