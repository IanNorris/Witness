#include "BuildIdentity.h"
#include "NativeBuildIdentity.h"
const char* GetNativeBuildIdentity() { return WITNESS_NATIVE_BUILD_ID; }
const char* GetNativeBuildConfiguration() { return WITNESS_NATIVE_CONFIGURATION; }
