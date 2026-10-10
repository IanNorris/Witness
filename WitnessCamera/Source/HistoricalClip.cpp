#include "HistoricalClip.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
extern "C" {
#include <libavformat/avformat.h>
}

namespace Witness::Camera {
namespace {
namespace fs = std::filesystem;
void Require(bool success, const char* message) { if (!success) throw std::runtime_error(message); }
struct Input {
    AVFormatContext* Context = nullptr;
    ~Input() { avformat_close_input(&Context); }
};
struct Output {
    AVFormatContext* Context = nullptr;
    ~Output() {
        if (Context) { if (Context->pb) avio_closep(&Context->pb); avformat_free_context(Context); }
    }
};
struct OwnedFiles {
    std::vector<fs::path> Paths;
    ~OwnedFiles() { for (const auto& path : Paths) { std::error_code ec; fs::remove(path, ec); } }
};
bool Compatible(const AVCodecParameters* a, const AVCodecParameters* b) {
    return a->codec_id == b->codec_id && a->width == b->width && a->height == b->height
        && a->sample_rate == b->sample_rate && a->ch_layout.nb_channels == b->ch_layout.nb_channels
        && a->extradata_size == b->extradata_size
        && (a->extradata_size == 0 || std::memcmp(a->extradata, b->extradata, a->extradata_size) == 0);
}
}

HistoricalClipResult RemuxHistoricalClip(const std::vector<HistoricalClipSource>& sources,
    double from, double to, const std::string& destination, const std::function<bool()>& cancelled)
{
    HistoricalClipResult result;
    OwnedFiles owned;
    Output output;
    try {
        Require(std::isfinite(from) && std::isfinite(to) && from >= 0 && to > from && to - from <= 21600,
            "Invalid capture window (maximum six hours)");
        Require(sources.size() <= 1024, "Too many capture sources");
        const fs::path target(destination), stage(destination + ".partial");
        Require(!fs::exists(target) && !fs::exists(stage) && fs::is_directory(target.parent_path()), "Capture destination is not a new file in an existing directory");
        auto ordered = sources;
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {return a.StartTimestamp < b.StartTimestamp;});
        int64_t lastDts[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
        int videoOutput = -1, audioOutput = -1;
        bool started = false;
        size_t sourceIndex = 0;
        for (const auto& source : ordered) {
            Require(std::isfinite(source.StartTimestamp) && std::isfinite(source.EndTimestamp)
                && source.StartTimestamp >= 0 && source.EndTimestamp > source.StartTimestamp && source.CommittedBytes >= 0, "Invalid source metadata");
            if (source.EndTimestamp <= from || source.StartTimestamp >= to) continue;
            if (cancelled && cancelled()) throw std::runtime_error("Capture cancelled");
            std::string path = source.FilePath;
            std::error_code ec;
            const auto size = fs::file_size(path, ec);
            if (ec) continue; // Missing/retained-away files remain explicit gaps.
            if (source.CommittedBytes > 0) {
                Require(static_cast<uint64_t>(source.CommittedBytes) <= size, "Committed source prefix is incomplete");
                const fs::path sealed(destination + ".source-" + std::to_string(sourceIndex++) + ".mp4");
                Require(!fs::exists(sealed), "Source snapshot destination already exists");
                std::ifstream inputFile(path, std::ios::binary);
                Require(inputFile.good(), "Could not read source snapshot");
                std::ofstream snapshot(sealed, std::ios::binary);
                Require(snapshot.good(), "Could not create source snapshot");
                owned.Paths.push_back(sealed);
                char buffer[65536];
                for (int64_t left = source.CommittedBytes; left > 0;) {
                    if (cancelled && cancelled()) throw std::runtime_error("Capture cancelled");
                    const auto count = (std::min<int64_t>)(left, sizeof(buffer));
                    inputFile.read(buffer, count);
                    Require(inputFile.gcount() == count, "Source snapshot read was incomplete");
                    snapshot.write(buffer, count);
                    Require(snapshot.good(), "Source snapshot write failed");
                    left -= count;
                }
                snapshot.close();
                Require(!snapshot.fail(), "Source snapshot close failed");
                path = sealed.string();
            }
            Input input;
            Require(avformat_open_input(&input.Context, path.c_str(), nullptr, nullptr) >= 0
                && avformat_find_stream_info(input.Context, nullptr) >= 0, "Could not read historical media");
            const int video = av_find_best_stream(input.Context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            const int audio = av_find_best_stream(input.Context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            Require(video >= 0, "Historical source has no video");
            const auto videoStream = input.Context->streams[video];
            const double origin = videoStream->start_time == AV_NOPTS_VALUE ? 0 : videoStream->start_time * av_q2d(videoStream->time_base);
            const double offset = (std::max)(0.0, from - source.StartTimestamp);
            if (offset > 0) Require(av_seek_frame(input.Context, video,
                static_cast<int64_t>((origin + offset) / av_q2d(videoStream->time_base)), AVSEEK_FLAG_BACKWARD) >= 0, "Historical seek failed");
            bool atKeyframe = false;
            double coverageFrom = 0, coverageTo = 0;
            std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) {av_packet_free(&p);});
            Require(packet != nullptr, "Packet allocation failed");
            int readResult = 0;
            while ((readResult = av_read_frame(input.Context, packet.get())) >= 0) {
                if (cancelled && cancelled()) throw std::runtime_error("Capture cancelled");
                AVPacket* p = packet.get();
                Require(p->size <= 32 * 1024 * 1024, "Historical packet exceeded size limit");
                const bool isVideo = p->stream_index == video;
                const bool isAudio = p->stream_index == audio && audio >= 0;
                if ((!isVideo && !isAudio) || p->dts == AV_NOPTS_VALUE || p->pts == AV_NOPTS_VALUE) {av_packet_unref(p); continue;}
                const auto stream = input.Context->streams[p->stream_index];
                const double dts = source.StartTimestamp + p->dts * av_q2d(stream->time_base) - origin;
                const double pts = source.StartTimestamp + p->pts * av_q2d(stream->time_base) - origin;
                if (isVideo && dts >= to) break;
                if (!atKeyframe) {
                    if (!isVideo || !(p->flags & AV_PKT_FLAG_KEY)) {av_packet_unref(p); continue;}
                    atKeyframe = true; coverageFrom = pts;
                }
                if (isAudio && (pts < coverageFrom || pts >= to)) {av_packet_unref(p); continue;}
                if (!started) {
                    Require(avformat_alloc_output_context2(&output.Context, nullptr, "mp4", stage.string().c_str()) >= 0 && output.Context, "Output allocation failed");
                    for (const int index : {video, audio}) {
                        if (index < 0) continue;
                        const auto out = avformat_new_stream(output.Context, nullptr);
                        Require(out && avcodec_parameters_copy(out->codecpar, input.Context->streams[index]->codecpar) >= 0, "Could not copy stream configuration");
                        out->codecpar->codec_tag = 0;
                        out->time_base = input.Context->streams[index]->time_base;
                        if (index == video) videoOutput = out->index; else audioOutput = out->index;
                    }
                    Require(avio_open(&output.Context->pb, stage.string().c_str(), AVIO_FLAG_WRITE) >= 0, "Could not open capture output");
                    owned.Paths.push_back(stage);
                    Require(avformat_write_header(output.Context, nullptr) >= 0, "Could not write capture header");
                    result.ActualFrom = pts; result.TimelineFrom = dts; started = true;
                }
                const int outIndex = isVideo ? videoOutput : audioOutput;
                Require(outIndex >= 0 && Compatible(output.Context->streams[outIndex]->codecpar, stream->codecpar), "Stream configuration changed within capture");
                const auto outStream = output.Context->streams[outIndex];
                p->pts = av_rescale_q(static_cast<int64_t>(std::llround((pts - result.TimelineFrom) * AV_TIME_BASE)), AV_TIME_BASE_Q, outStream->time_base);
                p->dts = av_rescale_q(static_cast<int64_t>(std::llround((dts - result.TimelineFrom) * AV_TIME_BASE)), AV_TIME_BASE_Q, outStream->time_base);
                p->duration = av_rescale_q(p->duration, stream->time_base, outStream->time_base);
                const int lane = isVideo ? 0 : 1;
                Require(lastDts[lane] == AV_NOPTS_VALUE || p->dts > lastDts[lane], "Overlapping or non-monotonic capture sources");
                lastDts[lane] = p->dts;
                if (isVideo) {
                    coverageTo = (std::max)(coverageTo, pts + p->duration * av_q2d(outStream->time_base));
                    ++result.VideoPackets;
                } else ++result.AudioPackets;
                p->stream_index = outIndex; p->pos = -1;
                Require(av_interleaved_write_frame(output.Context, p) >= 0, "Capture packet write failed");
                if (avio_tell(output.Context->pb) > 2LL * 1024 * 1024 * 1024) throw std::runtime_error("Capture exceeded two GiB limit");
                av_packet_unref(p);
            }
            Require(readResult == AVERROR_EOF || readResult >= 0, "Historical read failed before EOF");
            if (coverageTo > coverageFrom) result.Coverage.push_back({coverageFrom, coverageTo});
        }
        Require(started && result.VideoPackets > 0, "No playable video in requested window");
        Require(av_write_trailer(output.Context) >= 0 && output.Context->pb->error >= 0, "Capture finalization failed");
        Require(avio_closep(&output.Context->pb) >= 0, "Capture close failed");
        double cursor = from;
        for (const auto& range : result.Coverage) {
            if (range.From > cursor) result.Gaps.push_back({cursor, (std::min)(range.From, to)});
            cursor = (std::max)(cursor, range.To); result.ActualTo = (std::max)(result.ActualTo, range.To);
        }
        if (cursor < to) result.Gaps.push_back({cursor, to});
        // Both paths are in the same directory. Linking publishes the closed
        // file without the overwrite race of exists()+rename() on POSIX.
        fs::create_hard_link(stage, target);
        result.Success = true;
    } catch (const std::exception& error) {result.Error = error.what();}
    return result;
}
}
