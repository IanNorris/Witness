#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace Witness::Camera
{
	using SegmentBuffer = std::shared_ptr<std::vector<uint8_t>>;
}
