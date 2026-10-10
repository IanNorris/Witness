#pragma once
#include "Export.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Witness::Camera {
struct HistoricalClipSource {
    std::string FilePath;
    double StartTimestamp = 0;
    double EndTimestamp = 0;
    // Positive only for a trusted committed boundary published by the writer.
    int64_t CommittedBytes = 0;
    // Legacy DVR rows round wall-clock boundaries to whole seconds. Only
    // explicitly contiguous rows may reconcile that bounded uncertainty.
    double BoundaryToleranceSeconds = 0;
};
struct HistoricalClipRange { double From = 0; double To = 0; };
struct HistoricalClipResult {
    bool Success = false;
    std::string Error;
    double ActualFrom = 0;
    double ActualTo = 0;
    double TimelineFrom = 0;
    uint64_t VideoPackets = 0;
    uint64_t AudioPackets = 0;
    std::vector<HistoricalClipRange> Coverage;
    std::vector<HistoricalClipRange> Gaps;
};
// Blocking worker operation, never a camera/HTTP-handler operation. Sources
// and the destination are server-selected, not client-provided filesystem paths.
// Passthrough starts at a preceding RAP and may include a trailing reference
// picture; callers must use the returned actual boundaries, not claim exact cuts.
CAMERA_API HistoricalClipResult RemuxHistoricalClip(const std::vector<HistoricalClipSource>& sources,
    double from, double to, const std::string& destination, const std::function<bool()>& cancelled = {});
}
