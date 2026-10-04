#include "CpuUsage.h"
#include "CpuUsageJson.h"
#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Witness::Diagnostics;
using namespace std::chrono_literals;
static void Require(bool Value, const char* Message) { if (!Value) throw std::runtime_error(Message); }
static CpuCounters Baseline() {
    CpuCounters Value;
    Value.HostAvailable = Value.ProcessAvailable = true;
    Value.LogicalProcessors = 2; Value.MonotonicNs = 1000000000;
    Value.Idle = 4000000; Value.Kernel = 8000000; Value.User = 2000000;
    Value.ProcessKernel = Value.ProcessUser = 1000000;
    return Value;
}
static CpuCounters Active() {
    auto Value = Baseline(); Value.MonotonicNs += 1000000000;
    Value.Idle += 4000000; Value.Kernel += 12000000; Value.User += 8000000;
    Value.ProcessKernel += 2000000; Value.ProcessUser += 3000000;
    return Value;
}
int main() {
    try {
        const auto Before = Baseline(), After = Active();
        const auto Warmup = CalculateCpuUsage({}, Before);
        Require(!Warmup.HostPercent && !Warmup.ProcessPercent, "warmup is not measured zero");
        crow::json::wvalue Host;
        WriteCpuHealth(Host, Warmup);
        auto Json = crow::json::load(Host.dump());
        Require(Json && Json["cpuPercent"].t() == crow::json::type::Null &&
            Json["processCpuPercent"].t() == crow::json::type::Null &&
            Json["cpuUnavailableReason"].s() == "baselineRequired", "warmup JSON preserves null and reason");
        const auto Known = CalculateCpuUsage(Before, After);
        Require(Known.HostPercent && std::abs(*Known.HostPercent - 80) < 1e-9, "kernel includes idle exactly once");
        Require(Known.ProcessPercent && std::abs(*Known.ProcessPercent - 25) < 1e-9, "process normalized to all logical CPU capacity");
        Require(Known.WindowMs == 1000 && Known.AgeMs == 0, "explicit window");
        auto Value = After;
        Value.ProcessKernel = Before.ProcessKernel; Value.ProcessUser = Before.ProcessUser;
        Require(CalculateCpuUsage(Before, Value).ProcessPercent == 0, "idle process is measured zero");
        WriteCpuHealth(Host, CalculateCpuUsage(Before, Value));
        Json = crow::json::load(Host.dump());
        Require(Json && Json["processCpuPercent"].t() == crow::json::type::Number &&
            Json["processCpuPercent"].d() == 0 && Json["cpuSampleWindowMs"].d() == 1000 &&
            Json["processCpuUnavailableReason"].s() == "", "measured zero remains numeric in health JSON");
        Value.Kernel = Before.Kernel; Value.User = Before.User; Value.Idle = Before.Idle;
        Require(!CalculateCpuUsage(Before, Value).HostPercent, "no host counter progress is unavailable");
        for (int Scenario = 0; Scenario < 5; ++Scenario) {
            Value = After;
            if (Scenario == 0) Value.MonotonicNs = Before.MonotonicNs;
            if (Scenario == 1) Value.MonotonicNs = Before.MonotonicNs - 1;
            if (Scenario == 2) Value.LogicalProcessors = 4;
            if (Scenario == 3) Value.Kernel = Before.Kernel - 1;
            if (Scenario == 4) Value.Idle = Before.Idle + 30000000;
            Require(!CalculateCpuUsage(Before, Value).HostPercent, "invalid host delta must not wrap/clamp to fake percentage");
        }
        Value = After; Value.ProcessUser = Before.ProcessUser - 1;
        Require(!CalculateCpuUsage(Before, Value).ProcessPercent, "process counter reset unavailable");
        Value = After; Value.ProcessUser += 100000000;
        Require(!CalculateCpuUsage(Before, Value).ProcessPercent, "impossible process capacity unavailable");
        Value = After; Value.HostAvailable = false; Value.HostReason = "processorGroupsUnsupported";
        const auto Partial = CalculateCpuUsage(Before, Value);
        Require(!Partial.HostPercent && Partial.ProcessPercent && std::string(Partial.HostReason) == "processorGroupsUnsupported", "partial coverage reason retained");

        std::atomic<uint64_t> Time{1000000000}; std::atomic<unsigned> Reads{0};
        CpuUsageSampler Sampler([&] { ++Reads; return Reads == 1 ? Before : After; }, [&] { return Time.load(); });
        Require(!Sampler.Get().HostPercent, "sampler initial warmup");
        Time += 100000000;
        Require(Sampler.Get().AgeMs == 100 && Reads == 1, "polling reuses baseline");
        Time += 900000000;
        Require(Sampler.Get().HostPercent == Known.HostPercent && Reads == 2, "sample after window");
        std::vector<std::future<void>> Readers;
        for (int Index = 0; Index < 8; ++Index) Readers.push_back(std::async(std::launch::async, [&] {
            for (int Poll = 0; Poll < 1000; ++Poll) Require(Sampler.Get().HostPercent == Known.HostPercent, "concurrent readers share sample");
        }));
        for (auto& Reader : Readers) Reader.get();
        Require(Reads == 2, "HTTP/TUI bursts must not resample OS counters");
        Time = 1;
        Require(!Sampler.Get().HostPercent && Reads == 3, "clock reset rebaselines cache");

        // Smoke-test the actual platform producer, not a substitute OS mock.
        const auto First = SampleCpuUsage();
        Require(!First.HostPercent && !First.ProcessPercent, "platform producer starts with baseline, not fake zero");
        std::this_thread::sleep_for(550ms);
        const auto Platform = SampleCpuUsage();
#ifdef _WIN32
        Require(Platform.ProcessPercent && Platform.WindowMs >= 500, "Windows process CPU collected");
        Require(*Platform.ProcessPercent >= 0 && *Platform.ProcessPercent <= 100, "platform process range");
        Require(Platform.HostPercent || std::string(Platform.HostReason) == "processorGroupsUnsupported", "Windows host coverage");
#else
        Require(!Platform.HostPercent && !Platform.ProcessPercent && std::string(Platform.HostReason) == "platformNotImplemented", "Linux remains explicit unsupported stub");
#endif
        std::cout << "PASS CPU arithmetic, unavailable/reset cases, cached concurrent consumers and platform sampling; windowMs=" << Platform.WindowMs
            << " hostPercent=" << Platform.HostPercent.value_or(-1) << " processPercent=" << Platform.ProcessPercent.value_or(-1) << '\n';
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
