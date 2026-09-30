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
#include <atomic>
#include <cstdio>
#include <cstring>

static std::atomic<uint64_t> AudioDurationWarnings{0}, MissingPtsWarnings{0}, FfmpegErrors{0};
static void ProbeLog(void*, int level, const char* format, va_list args)
{
	char message[2048];
	vsnprintf(message, sizeof(message), format, args);
	if (strstr(message, "Packet duration:") && strstr(message, "stream 1")) ++AudioDurationWarnings;
	if (strstr(message, "pts has no value")) ++MissingPtsWarnings;
	if (level <= AV_LOG_ERROR) ++FfmpegErrors;
}

using namespace Witness::Camera;
static uint64_t Timestamp()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv)
{
	const char* url = std::getenv("WITNESS_PREVIEW_PROBE_URL");
	if (!url || (argc != 3 && argc != 4)) return 2;
	const int seconds = std::atoi(argv[1]);
	if (seconds < 10 || seconds > 1800) return 2;
	const int cameraId = argc == 4 ? std::atoi(argv[3]) : 8;
	if (cameraId <= 0) return 2;
	av_log_set_level(AV_LOG_WARNING);
	av_log_set_callback(ProbeLog);
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
	bool Captured = false;
	auto capture = [&]() {
		std::string directory;
		if (!live.StartPacketCapture(cameraId, "preview", 30, directory)) return false;
		std::cout << "captureDirectory=" << directory << std::endl;
		Captured = true;
		return true;
	};
	if (std::getenv("WITNESS_PROBE_CAPTURE") && CaptureDelay == 0 && !capture()) return 6;
	std::ofstream output(argv[2], std::ios::binary);
	if (!output) return 4;
	SegmentBuffer init;
	bool started = false;
	int generation = -1;
	bool changedGeneration = false;
	live.SetEventCallback([&](const LiveStreamEvent& event) {
		if (event.EventType == LiveStreamEvent::InitSegmentReady) {
			if (started && event.Generation != generation) changedGeneration = true;
			init = event.Data;
			generation = event.Generation;
		}
		if (changedGeneration || event.EventType != LiveStreamEvent::PartialReady || !event.Data) return;
		if (!started) {
			if (!event.Independent || !init) return;
			output.write(reinterpret_cast<const char*>(init->data()), init->size());
			started = true;
		}
		output.write(reinterpret_cast<const char*>(event.Data->data()), event.Data->size());
	});
	const auto start = std::chrono::steady_clock::now();
	auto report = [&]() {
	const auto stats = live.GetStreamingDiagnostics(false);
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
		<< " phaseMs=" << stats.VideoPhaseErrorMs
		<< " saturated=" << stats.TimestampCorrectionSaturatedPackets
		<< " muxErrors=" << stats.MuxErrorPackets
		<< " corruptionEvents=" << stats.DecodeCorruptionEvents
		<< " recoveryEvents=" << stats.DecodeRecoveryEvents
		<< " avSkewMs=" << stats.AudioVideoSkewMs
		<< " audioDurationWarnings=" << AudioDurationWarnings.load()
		<< " missingPtsWarnings=" << MissingPtsWarnings.load()
		<< " ffmpegErrors=" << FfmpegErrors.load() << std::endl;
	};
	auto nextReport = start + std::chrono::seconds(60);
	CameraStreamError result = CameraStreamError::Success;
	while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
		result = input.ProcessFrame(nullptr, nullptr, &live);
		if (result != CameraStreamError::Success || changedGeneration) break;
		if (std::getenv("WITNESS_PROBE_CAPTURE") && !Captured &&
			std::chrono::steady_clock::now() - start >= std::chrono::seconds(CaptureDelay) && !capture()) return 6;
		if (std::chrono::steady_clock::now() >= nextReport) {
			report();
			nextReport += std::chrono::seconds(60);
		}
	}
	report();
	std::cout << "readResult=" << static_cast<int>(result)
		<< " generationChanged=" << changedGeneration << '\n';
	return result == CameraStreamError::Success && !changedGeneration && output.good() && started ? 0 : 5;
}
