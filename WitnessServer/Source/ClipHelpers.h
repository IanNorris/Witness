#pragma once

#include "Common.h"

class GlobalContext;

StringT GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video );
void DeleteOldClips( const GlobalContext& Context, int DaysToDelete );
