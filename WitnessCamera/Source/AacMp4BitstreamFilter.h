#pragma once
#include <climits>
#include <cstring>

extern "C" {
#include <libavcodec/bsf.h>
}

namespace Witness::Camera {
// empty_moov disables movenc's automatic BSFs. Keep an explicit, per-output
// filter; never mutate the shared input packets used by recording/analysis.
class AacMp4BitstreamFilter {
public:
    ~AacMp4BitstreamFilter() { Reset(); }
    AacMp4BitstreamFilter() = default;
    AacMp4BitstreamFilter(const AacMp4BitstreamFilter&) = delete;
    AacMp4BitstreamFilter& operator=(const AacMp4BitstreamFilter&) = delete;

    void Reset() { av_bsf_free(&Context); }
    int Initialize(const AVCodecParameters* Parameters, AVRational TimeBase) {
        Reset();
        const auto* Filter = av_bsf_get_by_name("aac_adtstoasc");
        if (!Filter) return AVERROR_BSF_NOT_FOUND;
        int Result = av_bsf_alloc(Filter, &Context);
        if (Result >= 0) Result = avcodec_parameters_copy(Context->par_in, Parameters);
        if (Result >= 0) {
            Context->time_base_in = TimeBase;
            Result = av_bsf_init(Context);
        }
        if (Result < 0) Reset();
        return Result;
    }
    int Convert(AVPacket* Packet) {
        if (!Context) return AVERROR(EINVAL);
        // aac_adtstoasc is a one-input/one-output non-delaying filter. Its raw
        // AAC path preserves bytes and metadata; ADTS emits raw AAC plus ASC
        // side data, without changing timestamps or requiring a decoder.
        int Result = av_bsf_send_packet(Context, Packet);
        if (Result < 0) return Result;
        return av_bsf_receive_packet(Context, Packet);
    }
    int PrimeConfiguration(AVCodecParameters* Parameters, const AVPacket* Input) {
        AVPacket* Packet = av_packet_clone(Input);
        if (!Packet) return AVERROR(ENOMEM);
        int Result = Convert(Packet);
        if (Result >= 0) {
            size_t Size = 0;
            const auto* Config = av_packet_get_side_data(Packet, AV_PKT_DATA_NEW_EXTRADATA, &Size);
            if (!Config || Size == 0 || Size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
                Result = AVERROR_INVALIDDATA;
            else {
                auto* Copy = static_cast<uint8_t*>(av_mallocz(Size + AV_INPUT_BUFFER_PADDING_SIZE));
                if (!Copy) Result = AVERROR(ENOMEM);
                else {
                    std::memcpy(Copy, Config, Size);
                    av_freep(&Parameters->extradata);
                    Parameters->extradata = Copy;
                    Parameters->extradata_size = static_cast<int>(Size);
                }
            }
        }
        av_packet_free(&Packet);
        return Result;
    }

private:
    AVBSFContext* Context = nullptr;
};
}
