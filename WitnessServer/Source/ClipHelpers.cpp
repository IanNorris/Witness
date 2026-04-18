#include "ClipHelpers.h"
#include "GlobalContext.h"

#include <Log.h>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <winerror.h>
#endif

namespace fs = std::filesystem;

std::string GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video )
{
	std::stringstream Stream;
	Stream << CameraID;

	if( Video )
	{
		if( Manual )
		{
			Stream << "_Manual";
		}
		else
		{
			Stream << "_Auto";
		}
	}

	Stream << "_" << Timestamp;

	if( Video )
	{
		Stream << ".mp4";
	}
	else
	{
		Stream << ".jpg";
	}

	return (fs::path(Context.CachePath) / Stream.str()).string();
}

static bool DeleteClipFiles( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual )
{
	auto ThumbnailPath = GetClipName( Context, CameraID, Timestamp, Manual, false );
	auto VideoPath = GetClipName( Context, CameraID, Timestamp, Manual, true );

	std::error_code error;
	if( !std::filesystem::remove( ThumbnailPath, error ) )
	{
		if( error.value() == E_ACCESSDENIED )
		{
			return false;
		}
	}

	if( !std::filesystem::remove( VideoPath, error ) )
	{
		if( error.value() == E_ACCESSDENIED )
		{
			return false;
		}
	}

	return true;
}

struct ClipToDelete
{
	int64_t ClipID;
	int CameraID;
	int64_t Timestamp;
	bool Manual;
};

void DeleteOldClips( const GlobalContext& Context, int DaysToDelete )
{
	const static int SecondsInDay = 60 * 60 * 24;
	int64_t Timestamp = static_cast<int64_t>(GetUnixTimestamp()) - (DaysToDelete * SecondsInDay);

	int totalDeleted = 0;
	bool moreToDelete = true;

	while( moreToDelete )
	{
		SQLiteDatabaseQueryInstance SelectClipsToDelete( Context.Database, "SelectClipsToDelete" );
		SelectClipsToDelete->Bind( "@Timestamp", Timestamp );

		std::vector<ClipToDelete> ClipsToDelete;

		SelectClipsToDelete->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				ClipToDelete Clip;
				Clip.ClipID = query.GetColumnValueInt64(0);
				Clip.Timestamp = query.GetColumnValueInt64(1);
				Clip.CameraID = query.GetColumnValueInt(2);
				Clip.Manual = query.GetColumnValueInt(6) == 0;

				ClipsToDelete.push_back(Clip);

				return true;
			}
		);

		if( ClipsToDelete.empty() )
		{
			moreToDelete = false;
			break;
		}

		for( auto& Clip : ClipsToDelete )
		{
			// Delete detection data for this clip's time range
			{
				SQLiteDatabaseQueryInstance q( Context.Database, "DeleteDetectionFramesInRange" );
				q->Bind( "@CameraID", Clip.CameraID );
				q->Bind( "@TimestampFrom", static_cast<double>( Clip.Timestamp ) );
				q->Bind( "@TimestampTo", static_cast<double>( Clip.Timestamp + 300 ) ); // generous: 5 min max clip
				q->Execute( nullptr );
			}

			// Delete clip files from disk
			DeleteClipFiles( Context, Clip.CameraID, Clip.Timestamp, Clip.Manual );

			// Delete clip record (also deletes ClipTag and Trail rows)
			SQLiteDatabaseQueryInstance DeleteClipQuery( Context.Database, "DeleteClip" );
			DeleteClipQuery->Bind( "@ClipUID", Clip.ClipID );
			DeleteClipQuery->Execute(
				[&]( const SQLiteDatabaseQuery& query )
				{
					return true;
				}
			);
		}

		totalDeleted += (int)ClipsToDelete.size();

		// If we got fewer than the batch limit, we're done
		if( ClipsToDelete.size() < 500 )
			moreToDelete = false;
	}

	if( totalDeleted > 0 )
	{
		LOG_INFO( "Clip cleanup: deleted %d clips older than %d days.", totalDeleted, DaysToDelete );
	}
}

void DeleteOldContinuousSegments( const GlobalContext& Context, int DaysToDelete )
{
	const static int SecondsInDay = 60 * 60 * 24;
	int64_t Timestamp = static_cast<int64_t>(GetUnixTimestamp()) - (DaysToDelete * SecondsInDay);

	SQLiteDatabaseQueryInstance SelectSegments( Context.Database, "SelectContinuousSegmentsToDelete" );
	SelectSegments->Bind( "@Timestamp", Timestamp );

	struct SegmentToDelete
	{
		int64_t SegmentUID;
		std::string FilePath;
	};

	std::vector<SegmentToDelete> SegmentsToDelete;

	SelectSegments->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			SegmentToDelete Seg;
			Seg.SegmentUID = query.GetColumnValueInt64(0);
			const char* path = query.GetColumnValueText(1);
			Seg.FilePath = path ? path : "";
			SegmentsToDelete.push_back(Seg);
			return true;
		}
	);

	for( auto& Seg : SegmentsToDelete )
	{
		std::error_code ec;
		if( !Seg.FilePath.empty() )
		{
			fs::remove( Seg.FilePath, ec );
		}

		SQLiteDatabaseQueryInstance DeleteSeg( Context.Database, "DeleteContinuousSegment" );
		DeleteSeg->Bind( "@SegmentUID", Seg.SegmentUID );
		DeleteSeg->Execute( [](const SQLiteDatabaseQuery&) { return true; } );
	}

	if( SegmentsToDelete.size() )
	{
		LOG_INFO( "Deleted %zu continuous segments older than %d days.", SegmentsToDelete.size(), DaysToDelete );
	}
}

void BackfillContinuousSegmentFileSizes( const GlobalContext& Context )
{
	SQLiteDatabaseQueryInstance query( Context.Database, "SelectContinuousSegmentsWithNoFileSize" );

	struct SegmentToUpdate
	{
		int64_t SegmentUID;
		std::string FilePath;
	};

	std::vector<SegmentToUpdate> segments;
	query->Execute( [&]( const SQLiteDatabaseQuery& q )
	{
		SegmentToUpdate s;
		s.SegmentUID = q.GetColumnValueInt64(0);
		const char* path = q.GetColumnValueText(1);
		s.FilePath = path ? path : "";
		segments.push_back(s);
		return true;
	});

	int updated = 0;
	for( auto& seg : segments )
	{
		std::error_code ec;
		auto fsize = fs::file_size( seg.FilePath, ec );
		if( ec ) continue;

		SQLiteDatabaseQueryInstance update( Context.Database, "UpdateContinuousSegmentFileSize" );
		update->Bind( "@FileSize", static_cast<int64_t>(fsize) );
		update->Bind( "@SegmentUID", seg.SegmentUID );
		update->Execute( [](const SQLiteDatabaseQuery&) { return true; } );
		updated++;
	}

	if( updated > 0 )
	{
		LOG_INFO( "Backfilled file sizes for %d continuous segments.", updated );
	}
}

void CleanupOrphanedContinuousSegments( const GlobalContext& Context )
{
	fs::path continuousDir = fs::path(Context.CachePath) / "continuous";
	std::error_code ec;
	if( !fs::exists( continuousDir, ec ) ) return;

	int orphanedFiles = 0;
	int orphanedRows = 0;

	// Scan disk for files without DB entries
	for( auto& cameraDir : fs::directory_iterator(continuousDir, ec) )
	{
		if( !cameraDir.is_directory() ) continue;

		for( auto& entry : fs::directory_iterator(cameraDir.path(), ec) )
		{
			if( !entry.is_regular_file() ) continue;
			if( entry.path().extension() != ".mp4" ) continue;

			std::string filePath = entry.path().string();
			SQLiteDatabaseQueryInstance query( Context.Database, "SelectContinuousSegmentByFilePath" );
			query->Bind( "@FilePath", filePath.c_str() );

			bool found = false;
			query->Execute( [&]( const SQLiteDatabaseQuery& ) { found = true; return true; } );

			if( !found )
			{
				fs::remove( filePath, ec );
				orphanedFiles++;
			}
		}
	}

	// Scan DB for entries whose files no longer exist
	struct SegmentToDelete
	{
		int64_t SegmentUID;
		std::string FilePath;
	};

	std::vector<SegmentToDelete> allSegments;
	{
		SQLiteDatabaseQueryInstance query( Context.Database, "SelectContinuousSegmentsToDelete" );
		// Use a far-future timestamp to get ALL segments
		query->Bind( "@Timestamp", static_cast<int64_t>(std::numeric_limits<int64_t>::max()) );
		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			SegmentToDelete s;
			s.SegmentUID = q.GetColumnValueInt64(0);
			const char* path = q.GetColumnValueText(1);
			s.FilePath = path ? path : "";
			allSegments.push_back(s);
			return true;
		});
	}

	for( auto& seg : allSegments )
	{
		if( seg.FilePath.empty() || !fs::exists( seg.FilePath, ec ) )
		{
			SQLiteDatabaseQueryInstance del( Context.Database, "DeleteContinuousSegment" );
			del->Bind( "@SegmentUID", seg.SegmentUID );
			del->Execute( [](const SQLiteDatabaseQuery&) { return true; } );
			orphanedRows++;
		}
	}

	if( orphanedFiles > 0 || orphanedRows > 0 )
	{
		LOG_WARNING( "Crash recovery: removed %d orphaned files and %d stale DB entries.", orphanedFiles, orphanedRows );
	}
	else
	{
		LOG_INFO( "Crash recovery: no orphaned continuous segments found." );
	}
}

void EnforceQuotaContinuousSegments( const GlobalContext& Context, int64_t quotaBytes )
{
	if( quotaBytes <= 0 ) return;

	// Get current total size
	SQLiteDatabaseQueryInstance sizeQuery( Context.Database, "SelectContinuousTotalSize" );
	int64_t totalSize = 0;
	sizeQuery->Execute( [&]( const SQLiteDatabaseQuery& q )
	{
		totalSize = q.GetColumnValueInt64(2); // column 2 = SUM(FileSize)
		return true;
	});

	int deleted = 0;
	while( totalSize > quotaBytes )
	{
		SQLiteDatabaseQueryInstance oldest( Context.Database, "SelectOldestContinuousSegment" );

		int64_t segUID = 0;
		std::string filePath;
		bool found = false;
		oldest->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			segUID = q.GetColumnValueInt64(0);
			const char* path = q.GetColumnValueText(1);
			filePath = path ? path : "";
			found = true;
			return true;
		});

		if( !found ) break;

		// Get file size before deleting (for accurate tracking)
		int64_t fileSize = 0;
		std::error_code ec;
		if( !filePath.empty() )
		{
			auto fsize = fs::file_size( filePath, ec );
			if( !ec ) fileSize = static_cast<int64_t>(fsize);
			fs::remove( filePath, ec );
		}

		SQLiteDatabaseQueryInstance del( Context.Database, "DeleteContinuousSegment" );
		del->Bind( "@SegmentUID", segUID );
		del->Execute( [](const SQLiteDatabaseQuery&) { return true; } );

		totalSize -= fileSize;
		deleted++;
	}

	if( deleted > 0 )
	{
		LOG_INFO( "Quota enforcement: deleted %d oldest continuous segments (quota: %lld MB).", deleted, quotaBytes / (1024*1024) );
	}
}

void CheckDiskSpaceSafety( const GlobalContext& Context )
{
	std::error_code ec;
	auto spaceInfo = fs::space( Context.CachePath, ec );
	if( ec ) return;

	int64_t freeBytes = static_cast<int64_t>(spaceInfo.available);
	int64_t freeGB = freeBytes / (1024LL * 1024 * 1024);
	int64_t freeMB = freeBytes / (1024LL * 1024);

	if( freeBytes < 2LL * 1024 * 1024 * 1024 )
	{
		LOG_WARNING( "Low disk space: %lld MB free on cache drive.", freeMB );
	}

	// Emergency: delete oldest continuous segments when below 1GB
	if( freeBytes < 1LL * 1024 * 1024 * 1024 )
	{
		LOG_WARNING( "Emergency disk cleanup: only %lld MB free, pruning continuous segments.", freeMB );
		int deleted = 0;
		while( deleted < 50 ) // limit per cycle to avoid locking DB too long
		{
			SQLiteDatabaseQueryInstance oldest( Context.Database, "SelectOldestContinuousSegment" );
			int64_t segUID = 0;
			std::string filePath;
			bool found = false;
			oldest->Execute( [&]( const SQLiteDatabaseQuery& q )
			{
				segUID = q.GetColumnValueInt64(0);
				const char* path = q.GetColumnValueText(1);
				filePath = path ? path : "";
				found = true;
				return true;
			});

			if( !found ) break;

			std::error_code rmec;
			if( !filePath.empty() ) fs::remove( filePath, rmec );

			SQLiteDatabaseQueryInstance del( Context.Database, "DeleteContinuousSegment" );
			del->Bind( "@SegmentUID", segUID );
			del->Execute( [](const SQLiteDatabaseQuery&) { return true; } );
			deleted++;

			// Recheck space after batch
			auto newSpace = fs::space( Context.CachePath, rmec );
			if( !rmec && static_cast<int64_t>(newSpace.available) >= 1LL * 1024 * 1024 * 1024 ) break;
		}

		if( deleted > 0 )
		{
			LOG_WARNING( "Emergency cleanup: deleted %d continuous segments.", deleted );
		}
	}
}

void CleanupOldDetectionFrames( const GlobalContext& Context, int retentionDays )
{
	auto now = std::chrono::system_clock::now();
	auto cutoff = now - std::chrono::hours( 24 * retentionDays );
	double cutoffEpoch = static_cast<double>(
		std::chrono::duration_cast<std::chrono::seconds>( cutoff.time_since_epoch() ).count()
	);

	// Get camera IDs while holding the lock, then release for DB ops
	std::vector<int> cameraIds;
	{
		std::shared_lock<std::shared_mutex> lock( Context.Mutex );
		for( auto& [id, state] : Context.GetCameraMap() )
			cameraIds.push_back( id );
	}

	for( int cameraId : cameraIds )
	{
		SQLiteDatabaseQueryInstance query( Context.Database, "DeleteDetectionFramesBefore" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@Timestamp", cutoffEpoch );
		query->Execute( nullptr );
	}

	// Also clean up FaceCrop data older than retention period
	for( int cameraId : cameraIds )
	{
		SQLiteDatabaseQueryInstance query( Context.Database, "DeleteFaceCropsBefore" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@Timestamp", cutoffEpoch );
		query->Execute( nullptr );
	}
}
