#include "CpuUsage.h"
#include <cmath>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Witness::Diagnostics {
CpuUsage CalculateCpuUsage(const std::optional<CpuCounters>& Previous, const CpuCounters& Current) {
    CpuUsage Result;
    if (!Current.HostAvailable) Result.HostReason = Current.HostReason;
    if (!Current.ProcessAvailable) Result.ProcessReason = Current.ProcessReason;
    if (!Previous) return Result;
    if (Current.MonotonicNs <= Previous->MonotonicNs) {
        if (Current.HostAvailable) Result.HostReason = "clockReset";
        if (Current.ProcessAvailable) Result.ProcessReason = "clockReset";
        return Result;
    }
    Result.WindowMs = static_cast<double>(Current.MonotonicNs - Previous->MonotonicNs) / 1e6;
    const bool SameCapacity = Current.LogicalProcessors && Current.LogicalProcessors == Previous->LogicalProcessors;
    if (Current.HostAvailable && Previous->HostAvailable) {
        Result.HostReason = "counterReset";
        if (SameCapacity && Current.Kernel >= Previous->Kernel && Current.User >= Previous->User && Current.Idle >= Previous->Idle) {
            const uint64_t KernelDelta = Current.Kernel - Previous->Kernel;
            const uint64_t UserDelta = Current.User - Previous->User;
            const uint64_t IdleDelta = Current.Idle - Previous->Idle;
            if (KernelDelta <= std::numeric_limits<uint64_t>::max() - UserDelta) {
                const uint64_t TotalDelta = KernelDelta + UserDelta;
                Result.HostReason = TotalDelta ? "invalidCounters" : "noCounterProgress";
                // Kernel includes idle time; subtract it once, not twice.
                if (TotalDelta && IdleDelta <= TotalDelta) {
                    Result.HostPercent = 100.0 * static_cast<double>(TotalDelta - IdleDelta) / static_cast<double>(TotalDelta);
                    Result.HostReason = "available";
                }
            }
        }
    }
    if (Current.ProcessAvailable && Previous->ProcessAvailable) {
        Result.ProcessReason = "counterReset";
        if (SameCapacity && Current.ProcessKernel >= Previous->ProcessKernel && Current.ProcessUser >= Previous->ProcessUser) {
            const double UsedTicks = static_cast<double>(Current.ProcessKernel - Previous->ProcessKernel) +
                static_cast<double>(Current.ProcessUser - Previous->ProcessUser);
            // Windows counters use 100 ns units. 100% means the entire host's
            // logical CPU capacity, not one fully occupied core.
            const double CapacityTicks = static_cast<double>(Current.MonotonicNs - Previous->MonotonicNs) / 100.0 * Current.LogicalProcessors;
            const double Percent = 100.0 * UsedTicks / CapacityTicks;
            if (std::isfinite(Percent) && Percent >= 0.0 && Percent <= 100.0) {
                Result.ProcessPercent = Percent; Result.ProcessReason = "available";
            } else Result.ProcessReason = "invalidCounters";
        }
    }
    return Result;
}

CpuUsage CpuUsageSampler::Get() {
    std::lock_guard Lock(Mutex);
    const uint64_t Timestamp = Now();
    if (Previous && Timestamp >= Previous->MonotonicNs && Timestamp - Previous->MonotonicNs < 500000000) {
        auto Result = Cached;
        Result.AgeMs = static_cast<double>(Timestamp - Previous->MonotonicNs) / 1e6;
        return Result;
    }
    auto Current = Read();
    Current.MonotonicNs = Timestamp;
    Cached = CalculateCpuUsage(Previous, Current);
    Previous = Current;
    return Cached;
}

static CpuCounters ReadPlatformCpu() {
    CpuCounters Result;
#ifdef _WIN32
    Result.HostReason = Result.ProcessReason = "collectionFailed";
    Result.LogicalProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!Result.LogicalProcessors) { SYSTEM_INFO Info{}; GetSystemInfo(&Info); Result.LogicalProcessors = Info.dwNumberOfProcessors; }
    auto Ticks = [](const FILETIME& Value) { return (uint64_t(Value.dwHighDateTime) << 32) | Value.dwLowDateTime; };
    FILETIME Idle{}, Kernel{}, User{};
    // GetSystemTimes is group-scoped above 64 CPUs. Do not label that as host CPU.
    if (Result.LogicalProcessors > 64) Result.HostReason = "processorGroupsUnsupported";
    else if (GetSystemTimes(&Idle, &Kernel, &User)) {
        Result.HostAvailable = true;
        Result.Idle = Ticks(Idle); Result.Kernel = Ticks(Kernel); Result.User = Ticks(User);
    }
    FILETIME Created{}, Exited{}, ProcessKernel{}, ProcessUser{};
    if (GetProcessTimes(GetCurrentProcess(), &Created, &Exited, &ProcessKernel, &ProcessUser)) {
        Result.ProcessAvailable = true;
        Result.ProcessKernel = Ticks(ProcessKernel); Result.ProcessUser = Ticks(ProcessUser);
    }
#endif
    return Result;
}

CpuUsage SampleCpuUsage() {
    static CpuUsageSampler Sampler(ReadPlatformCpu, [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    });
    return Sampler.Get();
}
}
