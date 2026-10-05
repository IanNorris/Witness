#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Witness::Camera {
// Owned by the Camera DLL's private queue data. Snapshot sorting is done on a
// caller-owned copy after releasing queue/stat locks, never on the worker path.
struct ProcessingLatencyWindow {
    static constexpr size_t Capacity = 1024;
    std::array<int64_t, Capacity> Values{};
    size_t Count = 0;
    size_t Next = 0;
    uint64_t Total = 0;

    void Add(int64_t NS) {
        if (NS < 0) return;
        Values[Next] = NS;
        Next = (Next + 1) % Capacity;
        Count = (std::min)(Count + 1, Capacity);
        ++Total;
    }
    void Reset() { Count = Next = 0; Total = 0; }
    // Copy the newest retained tasks chronologically, without changing/sorting
    // the writer's ring. Sequence numbers are local to the owning stats epoch.
    size_t CopyNewest(int64_t* Output, size_t OutputCapacity, uint64_t& FirstSequence) const {
        FirstSequence = 0;
        if (!Output || !OutputCapacity || !Count) return 0;
        const size_t Copied = (std::min)(Count, OutputCapacity);
        const size_t Oldest = Count == Capacity ? Next : 0;
        const size_t Skip = Count - Copied;
        for (size_t Index = 0; Index < Copied; ++Index)
            Output[Index] = Values[(Oldest + Skip + Index) % Capacity];
        FirstSequence = Total - Copied + 1;
        return Copied;
    }
    void Sort() { std::sort(Values.begin(), Values.begin() + Count); }
    // Nearest-rank percentile; 0 for an empty window is not an available value.
    int64_t Percentile(unsigned Percent) const {
        if (!Count || Percent < 1 || Percent > 100) return 0;
        return Values[(Count * Percent + 99) / 100 - 1];
    }
};
}
