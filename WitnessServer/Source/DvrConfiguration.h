#pragma once

#include <charconv>
#include <string_view>

namespace Witness::Dvr {
inline constexpr int DefaultSegmentSeconds = 60;

inline bool ParseSegmentSeconds(std::string_view value, int& seconds)
{
    if (value.empty()) return false;
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
        || parsed < 30 || parsed > 900) return false;
    seconds = parsed;
    return true;
}
}
