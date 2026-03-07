#pragma once

#include "Common.h"
#include <cstdint>

class GlobalContext;

std::string GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video );
void DeleteOldClips( const GlobalContext& Context, int DaysToDelete );
void DeleteOldContinuousSegments( const GlobalContext& Context, int DaysToDelete );
void BackfillContinuousSegmentFileSizes( const GlobalContext& Context );
void CleanupOrphanedContinuousSegments( const GlobalContext& Context );
void EnforceQuotaContinuousSegments( const GlobalContext& Context, int64_t quotaBytes );
void CheckDiskSpaceSafety( const GlobalContext& Context );
void CleanupOldDetectionFrames( const GlobalContext& Context, int retentionDays );
