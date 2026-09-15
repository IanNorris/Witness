#pragma once
#include <string_view>

namespace CrowAuth
{
	constexpr bool AllowLocalStreamDebugAccess( std::string_view peer, bool forwarded )
	{
#if defined(_DEBUG)
		return !forwarded && (peer == "127.0.0.1" || peer == "::1" || peer == "::ffff:127.0.0.1");
#else
		return false;
#endif
	}
}
