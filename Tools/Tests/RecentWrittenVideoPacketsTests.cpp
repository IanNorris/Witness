#include "RecentWrittenVideoPackets.h"
#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>
using Witness::Camera::RecentWrittenVideoPackets;
static void Check(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}
int main() {
    try {
        RecentWrittenVideoPackets History;
        Check(!History.Contains(0), "empty history classified zero hash as duplicate");
        History.Record(true, 42);
        for (int Index = 0; Index < 1023; ++Index) History.Record(false, 42);
        Check(History.Contains(42), "packet expired before the 1024-disposition horizon");
        History.Record(false, 42);
        Check(!History.Contains(42), "packet survived beyond the 1024-disposition horizon");
        for (int Index = 0; Index < 33; ++Index) History.Record(true, Index);
        Check(!History.Contains(0) && History.Contains(1) && History.Contains(32), "32-video horizon differs");

        struct Entry { bool Audio, Written; std::uint64_t Hash; };
        std::deque<Entry> Legacy;
        RecentWrittenVideoPackets Candidate;
        std::mt19937_64 Random(0x5749544e455353);
        auto LegacyContains = [&](std::uint64_t Hash) {
            int Checked = 0;
            for (auto It = Legacy.rbegin(); It != Legacy.rend() && Checked < 32; ++It) {
                if (It->Audio || !It->Written) continue;
                ++Checked;
                if (It->Hash == Hash) return true;
            }
            return false;
        };
        for (int Index = 0; Index < 20000; ++Index) {
            // Include sparse-video stretches that evict functional hashes by
            // total dispositions rather than by the written-video count.
            Entry Value{(Random() % 3) == 0, (Random() % 4) != 0, Random() % 97};
            if (Index % 4000 > 2000) Value.Written = false;
            Legacy.push_back(Value);
            if (Legacy.size() > 1024) Legacy.pop_front();
            Candidate.Record(!Value.Audio && Value.Written, Value.Hash);
            for (auto Hash : {Value.Hash, std::uint64_t(0), std::uint64_t(96), std::uint64_t(1000)})
                Check(Candidate.Contains(Hash) == LegacyContains(Hash), "functional history differs from legacy diagnostic-ring lookup");
        }
        std::cout << "Functional packet history matches legacy horizons and dispositions\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
