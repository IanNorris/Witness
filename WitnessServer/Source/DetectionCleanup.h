#pragma once

#include <sqlite3.h>

#include <stop_token>
#include <string>

struct DetectionCleanupStats
{
	int FramesDeleted = 0;
	int FaceCropsDeleted = 0;
	int AssetsHandled = 0;
};

// Owns no SQLite connection. The caller must give it a private connection that
// is not used by HTTP handlers or other workers.
class DetectionCleanup
{
public:
	DetectionCleanup( sqlite3* Database, std::string CachePath );
	DetectionCleanupStats RunPass( double CutoffEpoch );

private:
	sqlite3* m_Database;
	std::string m_CachePath;
};

void RunDetectionCleanupWorker( std::stop_token Stop, const std::string& DatabasePath,
	const std::string& CachePath, int RetentionDays );
