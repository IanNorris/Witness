#pragma once
#include <cstdint>
#include <limits>

namespace Witness::Camera {
// Fixed-sample audio access units cannot overlap. Operates in sample units,
// not rounded millisecond ticks; callers must validate codec/frame length.
class AudioSampleClock {
public:
    void Reset() { _Next = -1; }
    bool Normalize(int64_t Source, int Samples, int64_t& Output) {
        return Normalize(Source, Samples, _Next, Output);
    }
    static bool Normalize(int64_t Source, int Samples, int64_t& Next, int64_t& Output) {
        if (Source < 0 || Samples <= 0 || Samples > 8192 ||
            Source > (std::numeric_limits<int64_t>::max)() - Samples) return false;
        // Reolink timestamps can wander by several hundred ms while the
        // encoded sample count remains steady. A four-frame boundary caused
        // false gap anchors, followed by avoidable backwards-clock drops.
        // Require a full 16 access units before treating phase as a gap/reset.
        const int64_t Tolerance = static_cast<int64_t>(Samples) * 16;
        if (Next < 0 || (Source > Next && Source - Next > Tolerance)) Next = Source;
        // A large backwards clock reset is not ordinary per-packet jitter.
        // Do not rewrite arbitrarily distant audio into the video epoch.
        if ((Source < Next && Next - Source > Tolerance) ||
            Next > (std::numeric_limits<int64_t>::max)() - Samples) return false;
        Output = Next;
        Next += Samples;
        return true;
    }
private:
    int64_t _Next = -1;
};

// Restricted to ordinary MPEG-4 AAC-LC ASC (no explicit frequency, PCE,
// core-coder dependency or extension). Unknown formats retain source timing.
inline int AacLcFrameSamples(const uint8_t* Config, int Bytes, int SampleRate) {
    static constexpr int Rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350};
    if (!Config || Bytes != 2 || (Config[0] >> 3) != 2) return 0;
    const int Frequency = ((Config[0] & 7) << 1) | (Config[1] >> 7);
    const int Channels = (Config[1] >> 3) & 15;
    if (Frequency >= 13 || Rates[Frequency] != SampleRate || Channels < 1 || Channels > 7 || (Config[1] & 3)) return 0;
    return (Config[1] & 4) ? 960 : 1024;
}
}
