// Read-only, bounded FFmpeg motion-vector sample. Pass the RTSP URL through
// WITNESS_MV_PROBE_URL so credentials are not exposed in the process command line.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/motion_vector.h>
}

struct ProbeStats {
    int decoded = 0;
    int withVectors = 0;
    int keyframes = 0;
    int64_t vectors = 0;
    int64_t moving = 0;
    std::map<int, int64_t> scales;
    std::map<int, int64_t> blockAreas;
    std::map<int, int64_t> legacyAccepted;
};

int main(int argc, char** argv) {
    const char* url = std::getenv("WITNESS_MV_PROBE_URL");
    if (!url || !*url || argc != 2) {
        std::cerr << "usage: MotionVectorScaleProbe CAMERA_ID (URL in WITNESS_MV_PROBE_URL)\n";
        return 2;
    }
    av_log_set_level(AV_LOG_QUIET); // Never leak the URL through FFmpeg diagnostics.
    AVFormatContext* format = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    if (avformat_open_input(&format, url, nullptr, &options) < 0) {
        std::cerr << "camera " << argv[1] << ": open failed\n";
        av_dict_free(&options);
        return 1;
    }
    av_dict_free(&options);
    if (avformat_find_stream_info(format, nullptr) < 0) {
        std::cerr << "camera " << argv[1] << ": stream info failed\n";
        avformat_close_input(&format);
        return 1;
    }
    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        std::cerr << "camera " << argv[1] << ": no video stream\n";
        avformat_close_input(&format);
        return 1;
    }
    const AVCodecParameters* params = format->streams[videoIndex]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(params->codec_id);
    AVCodecContext* codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec || avcodec_parameters_to_context(codec, params) < 0) {
        std::cerr << "camera " << argv[1] << ": decoder setup failed\n";
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return 1;
    }
    codec->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    if (avcodec_open2(codec, decoder, nullptr) < 0) {
        std::cerr << "camera " << argv[1] << ": decoder open failed\n";
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        return 1;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ProbeStats stats;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    int packets = 0;
    while (stats.decoded < 150 && packets < 1000 && std::chrono::steady_clock::now() < deadline && av_read_frame(format, packet) >= 0) {
        ++packets;
        if (packet->stream_index == videoIndex && avcodec_send_packet(codec, packet) >= 0) {
            while (avcodec_receive_frame(codec, frame) == 0) {
                ++stats.decoded;
                stats.keyframes += (frame->flags & AV_FRAME_FLAG_KEY) != 0;
                if (const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS)) {
                    ++stats.withVectors;
                    const auto* vectors = reinterpret_cast<const AVMotionVector*>(side->data);
                    const size_t count = side->size / sizeof(AVMotionVector);
                    for (size_t i = 0; i < count; ++i) {
                        const AVMotionVector& mv = vectors[i];
                        const int scale = mv.motion_scale;
                        ++stats.scales[scale];
                        ++stats.blockAreas[int(mv.w) * mv.h];
                        ++stats.vectors;
                        const int64_t squared = int64_t(mv.motion_x) * mv.motion_x + int64_t(mv.motion_y) * mv.motion_y;
                        stats.moving += squared > 0;
                        if (squared >= 4 && squared <= 128000) ++stats.legacyAccepted[scale];
                    }
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    std::cout << "camera " << argv[1] << " codec=" << avcodec_get_name(params->codec_id)
              << " size=" << codec->width << 'x' << codec->height
              << " decoded=" << stats.decoded << " keyframes=" << stats.keyframes
              << " mv_frames=" << stats.withVectors << " vectors=" << stats.vectors
              << " moving=" << stats.moving << '\n';
    for (const auto& [scale, count] : stats.scales) {
        std::cout << "  scale=" << scale << " vectors=" << count
                  << " legacy_accepted=" << stats.legacyAccepted[scale] << '\n';
    }
    for (const auto& [area, count] : stats.blockAreas) {
        std::cout << "  block_area=" << area << " vectors=" << count << '\n';
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return stats.withVectors > 0 ? 0 : 1;
}
