#pragma once

#include "Common.h"

class GlobalContext;

std::string GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video );
void DeleteOldClips( const GlobalContext& Context, int DaysToDelete );
