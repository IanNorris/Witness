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

int main(int argc, char** argv)
{
    if (argc != 2 && (argc != 3 || std::string(argv[2]) != "audio")) return 2;
    const bool expectAudio = argc == 3;
    try
    {
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
            std::vector<LiveStreamSegment> retainedSegments;
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
                    for (int frame = 0; frame < 100; ++frame)
                        Require(input.ProcessFrame(nullptr, nullptr, &live) == CameraStreamError::Success, "packet processing");
                    const auto snapshot = live.GetInitSnapshot();
                    Require(snapshot.Data && snapshot.Generation == generation, "init generation");
                    if (expectAudio) Require(!snapshot.AudioCodec.empty(), "init audio codec");
                    Require(live.GetInitGeneration() == generation, "generation getter");
                    Require(Hash(live.GetInitSegment()) == Hash(snapshot.Data), "atomic init snapshot");
                    live.GetSegments(retainedSegments);
                    Require(!retainedSegments.empty(), "segment backlog");
                    if (generation == 0)
                    {
                        retainedInit = snapshot;
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
        }
        std::cout << "8 cross-DLL media ownership/reconnect/shutdown cycles passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
