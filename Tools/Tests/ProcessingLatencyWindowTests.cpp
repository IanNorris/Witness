#include "ProcessingLatencyWindow.h"
#include "SourceStats.h"
#include <stdexcept>

static void Require(bool Good) { if (!Good) throw std::runtime_error("processing latency regression"); }
int main() {
    using Witness::Camera::ProcessingLatencyWindow;
    ProcessingLatencyWindow Window;
    Require(Window.Count == 0 && Window.Percentile(95) == 0);
    Window.Add(-1); Require(Window.Count == 0);
    Window.Add(0); Window.Sort(); Require(Window.Count == 1 && Window.Percentile(99) == 0);
    Window.Reset();
    for (int Value = 100; Value >= 1; --Value) Window.Add(Value);
    auto Snapshot = Window;
    Snapshot.Sort();
    Require(Snapshot.Percentile(50) == 50 && Snapshot.Percentile(95) == 95 && Snapshot.Percentile(99) == 99);
    Require(Window.Values[0] == 100); // Sorting never changes the writer's ring.
    Window.Reset();
    for (int Value = 1; Value <= 1100; ++Value) Window.Add(Value);
    Require(Window.Count == 1024 && Window.Total == 1100);
    std::array<int64_t, 1024> Raw{};
    uint64_t First = 0;
    Require(Window.CopyNewest(Raw.data(), Raw.size(), First) == 1024 && First == 77);
    for (size_t Index = 0; Index < Raw.size(); ++Index)
        Require(Raw[Index] == static_cast<int64_t>(Index + 77));
    Require(Window.CopyNewest(Raw.data(), 3, First) == 3 && First == 1098);
    Require(Raw[0] == 1098 && Raw[1] == 1099 && Raw[2] == 1100);
    Require(Window.CopyNewest(nullptr, 1024, First) == 0 && First == 0);
    Require(Window.CopyNewest(Raw.data(), 0, First) == 0 && First == 0);
    Window.Sort();
    Require(Window.Percentile(50) == 588 && Window.Percentile(95) == 1049 && Window.Percentile(99) == 1090);
    Require(Window.Percentile(100) == 1100);
    Window.Reset(); Require(Window.Count == 0 && Window.Total == 0);
    Require(Window.CopyNewest(Raw.data(), Raw.size(), First) == 0 && First == 0);
    Window.Add(5); Window.Add(3);
    Require(Window.CopyNewest(Raw.data(), Raw.size(), First) == 2 && First == 1);
    Require(Raw[0] == 5 && Raw[1] == 3);
    SourceStats Stats;
    Stats.PendingEssentialJobs = 3; Stats.ProcessingWorkSamples = 4;
    const auto Epoch = Stats.ProcessingWorkEpoch;
    Stats.Reset();
    Require(Stats.ProcessingWorkEpoch == Epoch + 1 && Stats.ProcessingWorkSamples == 0);
    Require(Stats.PendingEssentialJobs == 3);
}
