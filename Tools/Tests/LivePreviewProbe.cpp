// Bounded, opt-in real-camera mux probe. Supply a URL through the environment
// rather than command arguments so credentials never enter the command log.
#include "InputStream.h"
#include "LiveOutputStream.h"
extern "C" {
#include <libavutil/log.h>
}
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace Witness::Camera;
class LogTestStream : public Stream
{
public:
	CameraStreamError WriteInterleavedPacket(const AVPacket*) override { return CameraStreamError::Success; }
};
// Declared after the callback's local captures, so even an early return clears
// the callback before those locals die and the mux destructor flushes.
struct ProbeCallbackLifetime
{
	LiveOutputStream& Live;
	~ProbeCallbackLifetime() { Live.SetEventCallback({}); }
};
static uint64_t Timestamp()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--log-self-test") == 0) {
		LogTestStream stream;
		stream.Initialize();
		av_log_set_level(AV_LOG_WARNING);
		const auto before = GetFFmpegThreadLogCounts();
		FFmpegLogContextScope scope(9, "probe-self-test");
		av_log(nullptr, AV_LOG_WARNING, "Packet duration: stream 1\n");
		av_log(nullptr, AV_LOG_WARNING, "pts has no value\n");
		av_log(nullptr, AV_LOG_ERROR, "synthetic probe error\n");
		// Another stream initialization must not disable counting or HasError.
		LogTestStream second;
		second.Initialize();
		av_log(nullptr, AV_LOG_ERROR, "second synthetic probe error\n");
		const auto after = GetFFmpegThreadLogCounts();
		const bool passed = scope.HasError() && after.Errors == before.Errors + 2 &&
			after.AudioDurationWarnings == before.AudioDurationWarnings + 1 &&
			after.MissingPtsWarnings == before.MissingPtsWarnings + 1;
		std::cout << "logSelfTest=" << passed << std::endl;
		return passed ? 0 : 7;
	}
	const char* url = std::getenv("WITNESS_PREVIEW_PROBE_URL");
	if (!url || (argc != 3 && argc != 4)) return 2;
	const int seconds = std::atoi(argv[1]);
	if (seconds < 10 || seconds > 1800) return 2;
	const int cameraId = argc == 4 ? std::atoi(argv[3]) : 8;
	if (cameraId <= 0) return 2;
	av_log_set_level(AV_LOG_WARNING);
	InputStreamSetup setup;
	setup.GetTimestamp = Timestamp;
	setup.PassthroughOnly = true;
	setup.ExportMotionVectors = false;
	setup.HistoricalPacketBufferSeconds = 0;
	InputStream input(setup, cameraId, nullptr, url);
	if (input.Initialize() != CameraStreamError::Success) return 3;
	LiveOutputStream live("build-vs2026", &input, 1);
	live.SetObservedTimestampRegressionRepairAllowed(true);
	if (std::getenv("WITNESS_PROBE_REOLINK")) live.SetTimestampNormalizationAllowed(true);
	const int CaptureDelay = std::getenv("WITNESS_PROBE_CAPTURE_DELAY") ? std::atoi(std::getenv("WITNESS_PROBE_CAPTURE_DELAY")) : 0;
	const int CaptureEvery = std::getenv("WITNESS_PROBE_CAPTURE_EVERY") ? std::atoi(std::getenv("WITNESS_PROBE_CAPTURE_EVERY")) : 31;
	const int CaptureCount = std::getenv("WITNESS_PROBE_CAPTURE_COUNT") ? std::atoi(std::getenv("WITNESS_PROBE_CAPTURE_COUNT")) : 1;
	if (CaptureCount < 1 || CaptureCount > 8 || CaptureEvery < 31 || CaptureDelay < 0 ||
		(std::getenv("WITNESS_PROBE_CAPTURE") && static_cast<int64_t>(CaptureDelay) +
		 static_cast<int64_t>(CaptureEvery) * (CaptureCount - 1) + 30 > seconds)) return 2;
	const int PauseAt = std::getenv("WITNESS_PROBE_PAUSE_AT") ? std::atoi(std::getenv("WITNESS_PROBE_PAUSE_AT")) : -1;
	const int PauseSeconds = std::getenv("WITNESS_PROBE_PAUSE_SECONDS") ? std::atoi(std::getenv("WITNESS_PROBE_PAUSE_SECONDS")) : 0;
	if (PauseAt >= 0 && (PauseSeconds < 1 || PauseSeconds > 15 || PauseAt + PauseSeconds + 5 > seconds)) return 2;
	bool PauseInjected = false;
	int Captured = 0;
	auto capture = [&]() {
		std::string directory;
		if (!live.StartPacketCapture(cameraId, "preview", 30, directory)) return false;
		std::cout << "captureDirectory=" << directory << std::endl;
		++Captured;
		return true;
	};
	if (std::getenv("WITNESS_PROBE_CAPTURE") && CaptureDelay == 0 && !capture()) return 6;
	std::ofstream output(argv[2], std::ios::binary);
	if (!output) return 4;
	SegmentBuffer init;
	bool started = false;
	int generation = -1;
	bool changedGeneration = false;
	int discontinuities = 0;
	live.SetEventCallback([&](const LiveStreamEvent& event) {
		if (event.EventType == LiveStreamEvent::Discontinuity) ++discontinuities;
		if (event.EventType == LiveStreamEvent::InitSegmentReady) {
			if (generation >= 0 && event.Generation != generation) {
				changedGeneration = true;
				output.close();
				output.open(std::string(argv[2]) + ".generation-" +
					std::to_string(event.Generation) + ".mp4", std::ios::binary);
				started = false;
			}
			init = event.Data;
			generation = event.Generation;
		}
		if (event.EventType != LiveStreamEvent::PartialReady || !event.Data) return;
		if (!started) {
			if (!event.Independent || !init) return;
			output.write(reinterpret_cast<const char*>(init->data()), init->size());
			started = true;
		}
		output.write(reinterpret_cast<const char*>(event.Data->data()), event.Data->size());
	});
	ProbeCallbackLifetime callbackLifetime{live};
	const auto start = std::chrono::steady_clock::now();
	double LastReadMs = 0, MaxReadMs = 0;
	uint64_t ReadsOverOneSecond = 0;
	auto report = [&]() {
	const auto stats = live.GetStreamingDiagnostics(false);
	const auto logs = GetFFmpegThreadLogCounts();
	std::cout << "elapsedSeconds=" << std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - start).count()
		<< " accepted=" << stats.AcceptedVideoPackets
		<< " dropped=" << stats.DroppedVideoPackets
		<< " establishedDropped=" << stats.EstablishedDroppedVideoPackets
		<< " beforeEpoch=" << stats.BeforeVideoEpochPackets
		<< " negativeTimestamp=" << stats.NegativeTimestampPackets
		<< " nonMonotonicInput=" << stats.NonMonotonicInputPackets
		<< " nonMonotonicOutput=" << stats.NonMonotonicOutputPackets
		<< " repaired=" << stats.RepairedVideoTimestamps
		<< " normalized=" << stats.TimestampNormalizationActive
		<< " rejected=" << stats.TimestampNormalizationRejected
		<< " retries=" << stats.TimestampRetryAttempts
		<< " pauseResets=" << stats.TimestampPauseResets
		<< " lastPauseGapMs=" << stats.TimestampLastPauseGapMs
		<< " generation=" << stats.InitGeneration
		<< " discontinuities=" << discontinuities
		<< " rejectedDeclaredMs=" << stats.TimestampDeclaredWindowMs
		<< " rejectedReferenceMs=" << stats.TimestampReferenceWindowMs
		<< " deliveryPackets=" << stats.TimestampDelivery.Packets
		<< " deliveryDeclaredMs=" << stats.TimestampDelivery.DeclaredMs
		<< " deliveryMaxGapMs=" << stats.TimestampDelivery.MaxGapMs
		<< " deliveryLongGapExcessMs=" << stats.TimestampDelivery.LongGapExcessMs
		<< " deliveryInvalidDurations=" << stats.TimestampDelivery.InvalidDurations
		<< " deliveryDeclarationChanges=" << stats.TimestampDelivery.DeclarationChanges
		<< " phaseMs=" << stats.VideoPhaseErrorMs
		<< " saturated=" << stats.TimestampCorrectionSaturatedPackets
		<< " muxErrors=" << stats.MuxErrorPackets
		<< " corruptionEvents=" << stats.DecodeCorruptionEvents
		<< " recoveryEvents=" << stats.DecodeRecoveryEvents
		<< " avSkewMs=" << stats.AudioVideoSkewMs
		<< " lastProcessFrameMs=" << LastReadMs
		<< " maxProcessFrameMs=" << MaxReadMs
		<< " processFramesOverOneSecond=" << ReadsOverOneSecond
		<< " captures=" << Captured
		<< " audioDurationWarnings=" << logs.AudioDurationWarnings
		<< " missingPtsWarnings=" << logs.MissingPtsWarnings
		<< " ffmpegErrors=" << logs.Errors << std::endl;
	};
	auto nextReport = start + std::chrono::seconds(60);
	CameraStreamError result = CameraStreamError::Success;
	while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
		if (PauseAt >= 0 && !PauseInjected &&
			std::chrono::steady_clock::now() - start >= std::chrono::seconds(PauseAt)) {
			PauseInjected = true;
			std::cout << "probePauseSeconds=" << PauseSeconds << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(PauseSeconds));
		}
		if (std::getenv("WITNESS_PROBE_CAPTURE") && Captured < CaptureCount &&
			std::chrono::steady_clock::now() - start >=
			std::chrono::seconds(CaptureDelay + CaptureEvery * Captured) && !capture()) return 6;
		const auto readStart = std::chrono::steady_clock::now();
		result = input.ProcessFrame(nullptr, nullptr, &live);
		LastReadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
		if (LastReadMs > MaxReadMs) MaxReadMs = LastReadMs;
		if (LastReadMs >= 1000) ++ReadsOverOneSecond;
		if (result != CameraStreamError::Success || !output.good()) break;
		if (std::chrono::steady_clock::now() >= nextReport) {
			report();
			nextReport += std::chrono::seconds(60);
		}
	}
	// Flush while the output and all callback captures are still alive. A final
	// partial must not be written by the destructor through a dangling ofstream.
	live.Shutdown();
	live.SetEventCallback({});
	output.flush();
	report();
	std::cout << "readResult=" << static_cast<int>(result)
		<< " generationChanged=" << changedGeneration << '\n';
	return result == CameraStreamError::Success && output.good() && started ? 0 : 5;
}
