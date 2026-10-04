#pragma once
#include "CpuUsage.h"
#include <crow/json.h>

namespace Witness::Diagnostics {
inline void WriteCpuHealth(crow::json::wvalue& Host, const CpuUsage& Cpu) {
    Host["cpuPercent"] = Cpu.HostPercent ? crow::json::wvalue(*Cpu.HostPercent) : crow::json::wvalue(nullptr);
    Host["processCpuPercent"] = Cpu.ProcessPercent ? crow::json::wvalue(*Cpu.ProcessPercent) : crow::json::wvalue(nullptr);
    Host["cpuSampleWindowMs"] = Cpu.WindowMs;
    Host["cpuSampleAgeMs"] = Cpu.AgeMs;
    Host["cpuUnavailableReason"] = Cpu.HostPercent ? "" : Cpu.HostReason;
    Host["processCpuUnavailableReason"] = Cpu.ProcessPercent ? "" : Cpu.ProcessReason;
    Host["processCpuNormalization"] = "percentageOfHostLogicalCpuCapacity";
}
}
