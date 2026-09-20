#include "DetectionAssetFiles.h"

#include <Log.h>
#include <filesystem>

namespace fs = std::filesystem;

bool DeleteManagedDetectionAssets( const std::string& CachePath, int CameraID,
	const std::vector<std::string>& AssetPaths )
{
	std::error_code pathError;
	const fs::path absoluteCache = fs::absolute( CachePath, pathError );
	if( pathError )
	{
		LOG_WARNING( "Could not resolve the cache path for detection cleanup: %s", pathError.message().c_str() );
		return false;
	}
	const fs::path cachePath = fs::weakly_canonical( absoluteCache, pathError );
	if( pathError || cachePath.empty() )
	{
		LOG_WARNING( "Could not resolve the cache path for detection cleanup: %s", pathError.message().c_str() );
		return false;
	}

	bool filesDeleted = true;
	for( const auto& assetPath : AssetPaths )
	{
		if( assetPath.find( '\0' ) != std::string::npos )
		{
			LOG_WARNING( "Skipping removal of detection asset with an embedded NUL in its path." );
			continue;
		}
		std::error_code absoluteError;
		const fs::path unresolvedPath = fs::absolute( assetPath, absoluteError );
		if( absoluteError )
		{
			LOG_WARNING( "Skipping removal of unresolved detection asset path %s", assetPath.c_str() );
			continue;
		}
		const fs::path absolutePath = fs::weakly_canonical( unresolvedPath, absoluteError );
		const fs::path relative = absolutePath.lexically_relative( cachePath );
		if( absoluteError || relative.empty() )
		{
			LOG_WARNING( "Skipping removal of unresolved detection asset path %s", assetPath.c_str() );
			continue;
		}

		auto component = relative.begin();
		if( component == relative.end() || *component == ".." ||
			(*component != "frames" && *component != "crops" && *component != "faces") )
		{
			LOG_WARNING( "Skipping removal of unmanaged detection asset path %s", assetPath.c_str() );
			continue;
		}
		++component;
		if( component == relative.end() || *component != std::to_string( CameraID ) )
		{
			LOG_WARNING( "Skipping removal of detection asset outside camera %d: %s", CameraID, assetPath.c_str() );
			continue;
		}

		std::error_code ec;
		if( fs::is_directory( absolutePath, ec ) )
		{
			LOG_WARNING( "Skipping removal of detection asset directory %s", assetPath.c_str() );
			continue;
		}
		if( ec )
		{
			LOG_WARNING( "Failed to inspect detection asset %s: %s", assetPath.c_str(), ec.message().c_str() );
			filesDeleted = false;
			continue;
		}
		fs::remove( absolutePath, ec );
		if( ec )
		{
			LOG_WARNING( "Failed to remove expired detection asset %s: %s", assetPath.c_str(), ec.message().c_str() );
			filesDeleted = false;
		}
	}

	return filesDeleted;
}
