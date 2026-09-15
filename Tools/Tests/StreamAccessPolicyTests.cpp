// Compile with and without /D_DEBUG to exercise both build policies.
#include "../../WitnessServer/Source/StreamAccessPolicy.h"
using CrowAuth::AllowLocalStreamDebugAccess;
static_assert(!AllowLocalStreamDebugAccess("192.168.1.20", false));
static_assert(!AllowLocalStreamDebugAccess("203.0.113.20", false));
static_assert(!AllowLocalStreamDebugAccess("::ffff:192.168.1.20", false));
static_assert(!AllowLocalStreamDebugAccess("", false));
static_assert(!AllowLocalStreamDebugAccess("localhost", false));
static_assert(!AllowLocalStreamDebugAccess("127.0.0.1.example.com", false));
static_assert(!AllowLocalStreamDebugAccess("127.0.0.1", true));
static_assert(!AllowLocalStreamDebugAccess("::1", true));
static_assert(!AllowLocalStreamDebugAccess("::ffff:127.0.0.1", true));
#if defined(_DEBUG)
static_assert(AllowLocalStreamDebugAccess("127.0.0.1", false));
static_assert(AllowLocalStreamDebugAccess("::1", false));
static_assert(AllowLocalStreamDebugAccess("::ffff:127.0.0.1", false));
#else
static_assert(!AllowLocalStreamDebugAccess("127.0.0.1", false));
static_assert(!AllowLocalStreamDebugAccess("::1", false));
static_assert(!AllowLocalStreamDebugAccess("::ffff:127.0.0.1", false));
#endif
