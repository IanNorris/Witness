#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace Witness::Camera {
// Raw-log flood protection is independent of optional diagnostic history.
// The owner holds its stream mutex. No raw messages or payloads are retained.
class FFmpegLogClusters {
public:
    static constexpr size_t Capacity = 128;
    uint64_t Record(bool Error, std::string_view Phase, std::string_view Component,
                    std::string_view Signature, int Generation, int64_t ElapsedMs) {
        for (size_t Offset = 0; Offset < Count; ++Offset) {
            auto& Previous = Entries[(Next + Capacity - 1 - Offset) % Capacity];
            if (Previous.Error != Error || Previous.Phase != Phase ||
                Previous.Component != Component || Previous.Signature != Signature ||
                Previous.Generation != Generation) continue;
            const auto SinceLast = ElapsedMs - Previous.LastMs;
            const auto Age = ElapsedMs - Previous.FirstMs;
            if (SinceLast < 0 || SinceLast > 60000 || Age < 0 || Age > 300000) continue;
            Previous.LastMs = ElapsedMs;
            return ++Previous.Occurrences;
        }
        Entries[Next] = {Error, std::string(Phase), std::string(Component),
                        std::string(Signature), Generation, ElapsedMs, ElapsedMs, 1};
        Next = (Next + 1) % Capacity;
        if (Count < Capacity) ++Count;
        return 1;
    }
    size_t Size() const { return Count; }
private:
    struct Entry {
        bool Error = false;
        std::string Phase, Component, Signature;
        int Generation = 0;
        int64_t FirstMs = 0, LastMs = 0;
        uint64_t Occurrences = 0;
    };
    std::array<Entry, Capacity> Entries{};
    size_t Next = 0, Count = 0;
};
}
