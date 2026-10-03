// Offline baseline reproducer: supply a >=140s, 20fps, no-B-frame video fixture.
// Virtual elapsed time lets the actual mux exercise a 15s buffered pause/burst
// at a validation boundary without waiting two minutes or using camera secrets.
#include "InputStream.h"
#include "LiveOutputStream.h"
#include "StreamData.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace Witness::Camera;
namespace Witness::Camera {
struct LiveTimestampRecoveryTestAccess {
    static void Elapsed(LiveOutputStream& Live, int64_t Ms) {
        Live._PacketDiagEpoch = std::chrono::steady_clock::now() - std::chrono::milliseconds(Ms);
    }
};
}

static LiveOutputStream::StreamingDiagnostics Run(const char* Fixture, bool Pause) {
    InputStreamSetup Setup;
    Setup.GetTimestamp = []() -> uint64_t { return 1; };
    Setup.PassthroughOnly = true;
    Setup.ExportMotionVectors = false;
    Setup.HistoricalPacketBufferSeconds = 0;
    InputStream Input(Setup, 9, nullptr, Fixture);
    if (Input.Initialize() != CameraStreamError::Success) throw std::runtime_error("fixture initialization");
    // This harness replaces packet timestamps, so use the camera's 90kHz
    // clock rather than the fixture MP4's encoder-selected timebase.
    Input.GetData().FormatContext->streams[Input.GetData().ChosenStreamIndex]->time_base = {1, 90000};
    LiveOutputStream Live("", &Input, 1);
    Live.SetObservedTimestampRegressionRepairAllowed(true);
    AVPacket* Packet = av_packet_alloc();
    int Frames = 0;
    while (Frames < 2800 && av_read_frame(Input.GetData().FormatContext, Packet) >= 0) {
        if (Packet->stream_index == Input.GetData().ChosenStreamIndex) {
            // Distinct access units remain in order; only the declared source
            // clock has the Tapo-like one-second backward step.
            Packet->dts = Packet->pts = Frames * 4500LL - (Frames / 20) * 30000LL;
            Packet->duration = 4500;
            int64_t Ms = Frames * 50LL;
            if (Pause && Frames >= 2300 && Ms < 130000) Ms = 130000;
            LiveTimestampRecoveryTestAccess::Elapsed(Live, Ms);
            Live.BeginDiagnosticActivity();
            if (Live.WriteInterleavedPacket(Packet) != CameraStreamError::Success)
                throw std::runtime_error("mux failure");
            ++Frames;
        }
        av_packet_unref(Packet);
    }
    av_packet_free(&Packet);
    if (Frames != 2800) throw std::runtime_error("fixture too short");
    return Live.GetStreamingDiagnostics(false);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto Control = Run(argv[1], false);
    const auto Paused = Run(argv[1], true);
    if (Control.TimestampNormalizationRejected || !Control.TimestampNormalizationActive ||
        !Paused.TimestampNormalizationRejected || Paused.TimestampDelivery.LongGapExcessMs < 14000)
        return 1;
    std::cout << "REPRODUCED: steady control stays normalized; buffered pause rejects clock"
              << " declaredMs=" << Paused.TimestampDeclaredWindowMs
              << " referenceMs=" << Paused.TimestampReferenceWindowMs
              << " maxGapMs=" << Paused.TimestampDelivery.MaxGapMs
              << " longGapExcessMs=" << Paused.TimestampDelivery.LongGapExcessMs << '\n';
}
