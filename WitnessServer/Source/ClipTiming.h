#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

// An unset motion end means motion was still active when the recording ended.
// A real earlier end must retain the quiet/grace-period distinction.
inline uint64_t FinalizedMotionDurationSeconds(uint64_t ClipEnd, uint64_t MotionStart, uint64_t MotionEnd)
{
    if (MotionStart == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) || ClipEnd <= MotionStart)
        return 0;
    const auto End = MotionEnd == 0 ? ClipEnd : MotionEnd;
    return std::clamp(End, MotionStart, ClipEnd) - MotionStart;
}
