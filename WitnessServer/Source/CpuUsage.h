#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace Witness::Diagnostics {
struct CpuCounters {
    uint64_t Idle = 0, Kernel = 0, User = 0, ProcessKernel = 0, ProcessUser = 0;
    uint64_t MonotonicNs = 0;
    unsigned LogicalProcessors = 0;
    bool HostAvailable = false, ProcessAvailable = false;
    const char* HostReason = "platformNotImplemented";
    const char* ProcessReason = "platformNotImplemented";
};
struct CpuUsage {
    std::optional<double> HostPercent, ProcessPercent;
    const char* HostReason = "baselineRequired";
    const char* ProcessReason = "baselineRequired";
    double WindowMs = 0, AgeMs = 0;
};
CpuUsage CalculateCpuUsage(const std::optional<CpuCounters>& Previous, const CpuCounters& Current);

// One process-wide producer shared by terminal and HTTP consumers. Frequent
// readers reuse a sample, rather than consuming each other's tiny delta windows.
class CpuUsageSampler {
    std::mutex Mutex;
    std::function<CpuCounters()> Read;
    std::function<uint64_t()> Now;
    std::optional<CpuCounters> Previous;
    CpuUsage Cached;
public:
    CpuUsageSampler(std::function<CpuCounters()> Reader, std::function<uint64_t()> Clock)
        : Read(std::move(Reader)), Now(std::move(Clock)) {}
    CpuUsage Get();
};
CpuUsage SampleCpuUsage();
}
