#include "LiveOutputStream.h"
#include "NativeBuildIdentity.h"

const char* Witness::Camera::LiveOutputStream::GetNativeBuildIdentity() {
    return WITNESS_NATIVE_BUILD_ID;
}
const char* Witness::Camera::LiveOutputStream::GetNativeBuildConfiguration() {
    return WITNESS_NATIVE_CONFIGURATION;
}
