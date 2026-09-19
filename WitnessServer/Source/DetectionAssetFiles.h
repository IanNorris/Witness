#pragma once

#include <string>
#include <vector>

// Only files beneath a managed detection directory for this camera may be removed.
// Missing and unsafe paths are considered handled; filesystem failures are retried.
bool DeleteManagedDetectionAssets( const std::string& CachePath, int CameraID,
	const std::vector<std::string>& AssetPaths );
