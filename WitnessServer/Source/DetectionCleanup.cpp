#include "DetectionCleanup.h"
#include "DetectionAssetFiles.h"

#include <Log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	constexpr int CandidateBatchSize = 100;
	constexpr int FileBatchSize = 100;
	constexpr int FrameKind = 0;
	constexpr int FaceCropKind = 1;

	void Check( sqlite3* database, int result, const char* action )
	{
		if( result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW )
			throw std::runtime_error( std::string( action ) + ": " + sqlite3_errmsg( database ) );
	}

	class Statement
	{
	public:
		Statement( sqlite3* database, const char* sql ) : m_Database( database )
		{
			Check( database, sqlite3_prepare_v2( database, sql, -1, &m_Statement, nullptr ), "prepare cleanup SQL" );
		}
		~Statement() { sqlite3_finalize( m_Statement ); }
		Statement( const Statement& ) = delete;
		Statement& operator=( const Statement& ) = delete;

		void Bind( int index, int value ) { Check( m_Database, sqlite3_bind_int( m_Statement, index, value ), "bind int" ); }
		void Bind( int index, int64_t value ) { Check( m_Database, sqlite3_bind_int64( m_Statement, index, value ), "bind int64" ); }
		void Bind( int index, double value ) { Check( m_Database, sqlite3_bind_double( m_Statement, index, value ), "bind double" ); }
		void Bind( int index, const std::string& value )
		{
			Check( m_Database, sqlite3_bind_text( m_Statement, index, value.c_str(), -1, SQLITE_TRANSIENT ), "bind text" );
		}
		bool Row()
		{
			const int result = sqlite3_step( m_Statement );
			Check( m_Database, result, "step cleanup SQL" );
			return result == SQLITE_ROW;
		}
		void Run()
		{
			if( Row() ) throw std::runtime_error( "cleanup statement unexpectedly returned a row" );
		}
		int Int( int column ) const { return sqlite3_column_int( m_Statement, column ); }
		int64_t Int64( int column ) const { return sqlite3_column_int64( m_Statement, column ); }
		double Double( int column ) const { return sqlite3_column_double( m_Statement, column ); }
		std::string Text( int column ) const
		{
			const auto* value = sqlite3_column_text( m_Statement, column );
			return value ? std::string( reinterpret_cast<const char*>( value ),
				static_cast<size_t>( sqlite3_column_bytes( m_Statement, column ) ) ) : "";
		}

	private:
		sqlite3* m_Database;
		sqlite3_stmt* m_Statement = nullptr;
	};

	void Exec( sqlite3* database, const char* sql )
	{
		Check( database, sqlite3_exec( database, sql, nullptr, nullptr, nullptr ), sql );
	}

	class Transaction
	{
	public:
		explicit Transaction( sqlite3* database ) : m_Database( database ) { Exec( database, "BEGIN IMMEDIATE" ); }
		~Transaction() { if( m_Active ) sqlite3_exec( m_Database, "ROLLBACK", nullptr, nullptr, nullptr ); }
		void Commit() { Exec( m_Database, "COMMIT" ); m_Active = false; }
		Transaction( const Transaction& ) = delete;
		Transaction& operator=( const Transaction& ) = delete;
	private:
		sqlite3* m_Database;
		bool m_Active = true;
	};

	struct Cursor
	{
		double Timestamp = -std::numeric_limits<double>::max();
		int64_t UID = 0;
		bool Stored = false;
	};

	struct Candidate
	{
		int64_t UID;
		double Timestamp;
		bool Eligible;
	};

	Cursor LoadCursor( sqlite3* database, int cameraID, int kind )
	{
		Statement query( database, "SELECT CursorTimestamp,CursorUID FROM DetectionCleanupProgress WHERE CameraID=?1 AND Kind=?2" );
		query.Bind( 1, cameraID );
		query.Bind( 2, kind );
		Cursor cursor;
		if( query.Row() )
		{
			cursor.Timestamp = query.Double( 0 );
			cursor.UID = query.Int64( 1 );
			cursor.Stored = true;
		}
		return cursor;
	}

	void StoreCursor( sqlite3* database, int cameraID, int kind, const std::vector<Candidate>& page )
	{
		if( page.empty() )
		{
			Statement clear( database, "DELETE FROM DetectionCleanupProgress WHERE CameraID=?1 AND Kind=?2" );
			clear.Bind( 1, cameraID );
			clear.Bind( 2, kind );
			clear.Run();
			return;
		}
		const Candidate& last = page.back();
		Statement store( database,
			"INSERT INTO DetectionCleanupProgress(CameraID,Kind,CursorTimestamp,CursorUID) VALUES(?1,?2,?3,?4) "
			"ON CONFLICT(CameraID,Kind) DO UPDATE SET CursorTimestamp=excluded.CursorTimestamp,CursorUID=excluded.CursorUID" );
		store.Bind( 1, cameraID );
		store.Bind( 2, kind );
		store.Bind( 3, last.Timestamp );
		store.Bind( 4, last.UID );
		store.Run();
	}

	constexpr const char* FramePageSQL = R"SQL(
		WITH candidate AS MATERIALIZED (
			SELECT FrameUID,Timestamp FROM DetectionFrame
			WHERE CameraID=?1 AND Timestamp<?2 AND (Timestamp,FrameUID)>(?3,?4)
			ORDER BY Timestamp,FrameUID LIMIT ?5
		)
		SELECT p.FrameUID,p.Timestamp,
			NOT EXISTS(SELECT 1 FROM Clip c WHERE c.Camera=?1 AND p.Timestamp>=c.Timestamp
				AND p.Timestamp<=c.Timestamp+CASE WHEN COALESCE(c.Duration,0)>0 THEN c.Duration ELSE 1 END)
			AND NOT EXISTS(SELECT 1 FROM ContinuousSegment s WHERE s.CameraUID=?1
				AND p.Timestamp>=s.StartTimestamp AND p.Timestamp<=s.EndTimestamp)
		FROM candidate p ORDER BY p.Timestamp,p.FrameUID
	)SQL";

	constexpr const char* FaceCropPageSQL = R"SQL(
		WITH candidate AS MATERIALIZED (
			SELECT CropUID,Timestamp FROM FaceCrop
			WHERE CameraID=?1 AND Timestamp<?2 AND (Timestamp,CropUID)>(?3,?4)
			ORDER BY Timestamp,CropUID LIMIT ?5
		)
		SELECT p.CropUID,p.Timestamp,
			NOT EXISTS(SELECT 1 FROM FaceEmbedding fe WHERE fe.FaceCropUID=p.CropUID AND fe.Verified=1)
			AND NOT EXISTS(SELECT 1 FROM Clip c WHERE c.Camera=?1 AND p.Timestamp>=c.Timestamp
				AND p.Timestamp<=c.Timestamp+CASE WHEN COALESCE(c.Duration,0)>0 THEN c.Duration ELSE 1 END)
			AND NOT EXISTS(SELECT 1 FROM ContinuousSegment s WHERE s.CameraUID=?1
				AND p.Timestamp>=s.StartTimestamp AND p.Timestamp<=s.EndTimestamp)
		FROM candidate p ORDER BY p.Timestamp,p.CropUID
	)SQL";

	std::vector<Candidate> SelectPage( sqlite3* database, int cameraID, int kind,
		double cutoffEpoch, const Cursor& cursor )
	{
		Statement query( database, kind == FrameKind ? FramePageSQL : FaceCropPageSQL );
		query.Bind( 1, cameraID );
		query.Bind( 2, cutoffEpoch );
		query.Bind( 3, cursor.Timestamp );
		query.Bind( 4, cursor.UID );
		query.Bind( 5, CandidateBatchSize );
		std::vector<Candidate> page;
		while( query.Row() )
			page.push_back( { query.Int64( 0 ), query.Double( 1 ), query.Int( 2 ) != 0 } );
		return page;
	}

	constexpr const char* FrameStillEligibleSQL = R"SQL(
		SELECT 1 FROM DetectionFrame f WHERE f.FrameUID=?1 AND f.CameraID=?2 AND f.Timestamp<?3
			AND NOT EXISTS(SELECT 1 FROM Clip c WHERE c.Camera=?2 AND f.Timestamp>=c.Timestamp
				AND f.Timestamp<=c.Timestamp+CASE WHEN COALESCE(c.Duration,0)>0 THEN c.Duration ELSE 1 END)
			AND NOT EXISTS(SELECT 1 FROM ContinuousSegment s WHERE s.CameraUID=?2
				AND f.Timestamp>=s.StartTimestamp AND f.Timestamp<=s.EndTimestamp)
	)SQL";

	constexpr const char* FaceCropStillEligibleSQL = R"SQL(
		SELECT 1 FROM FaceCrop fc WHERE fc.CropUID=?1 AND fc.CameraID=?2 AND fc.Timestamp<?3
			AND NOT EXISTS(SELECT 1 FROM FaceEmbedding fe WHERE fe.FaceCropUID=fc.CropUID AND fe.Verified=1)
			AND NOT EXISTS(SELECT 1 FROM Clip c WHERE c.Camera=?2 AND fc.Timestamp>=c.Timestamp
				AND fc.Timestamp<=c.Timestamp+CASE WHEN COALESCE(c.Duration,0)>0 THEN c.Duration ELSE 1 END)
			AND NOT EXISTS(SELECT 1 FROM ContinuousSegment s WHERE s.CameraUID=?2
				AND fc.Timestamp>=s.StartTimestamp AND fc.Timestamp<=s.EndTimestamp)
	)SQL";

	bool StillEligible( sqlite3* database, int cameraID, int kind, int64_t uid, double cutoffEpoch )
	{
		Statement query( database, kind == FrameKind ? FrameStillEligibleSQL : FaceCropStillEligibleSQL );
		query.Bind( 1, uid );
		query.Bind( 2, cameraID );
		query.Bind( 3, cutoffEpoch );
		return query.Row();
	}

	void RunForID( sqlite3* database, const char* sql, int64_t uid )
	{
		Statement query( database, sql );
		query.Bind( 1, uid );
		query.Run();
	}

	void DeleteFrame( sqlite3* database, int cameraID, int64_t uid )
	{
		Statement framePath( database,
			"INSERT OR IGNORE INTO DetectionAssetDeletePending(Path,CameraID) "
			"SELECT FramePath,CameraID FROM DetectionFrame WHERE FrameUID=?1 AND CameraID=?2 AND FramePath IS NOT NULL AND FramePath<>''" );
		framePath.Bind( 1, uid );
		framePath.Bind( 2, cameraID );
		framePath.Run();

		Statement boxPaths( database,
			"INSERT OR IGNORE INTO DetectionAssetDeletePending(Path,CameraID) "
			"SELECT b.CropPath,f.CameraID FROM DetectionBox b JOIN DetectionFrame f ON f.FrameUID=b.FrameUID "
			"WHERE f.FrameUID=?1 AND f.CameraID=?2 AND b.CropPath IS NOT NULL AND b.CropPath<>''" );
		boxPaths.Bind( 1, uid );
		boxPaths.Bind( 2, cameraID );
		boxPaths.Run();

		// A verified or otherwise protected crop can outlive its source frame.
		RunForID( database, "UPDATE FaceCrop SET FrameUID=NULL WHERE FrameUID=?1", uid );
		RunForID( database, "DELETE FROM DetectionBox WHERE FrameUID=?1", uid );
		Statement remove( database, "DELETE FROM DetectionFrame WHERE FrameUID=?1 AND CameraID=?2" );
		remove.Bind( 1, uid );
		remove.Bind( 2, cameraID );
		remove.Run();
		if( sqlite3_changes( database ) != 1 ) throw std::runtime_error( "eligible detection frame vanished before deletion" );
	}

	void DeleteFaceCrop( sqlite3* database, int cameraID, int64_t uid )
	{
		Statement path( database,
			"INSERT OR IGNORE INTO DetectionAssetDeletePending(Path,CameraID) "
			"SELECT FilePath,CameraID FROM FaceCrop WHERE CropUID=?1 AND CameraID=?2 AND FilePath<>''" );
		path.Bind( 1, uid );
		path.Bind( 2, cameraID );
		path.Run();
		RunForID( database, "DELETE FROM FaceEmbedding WHERE FaceCropUID=?1", uid );
		Statement remove( database, "DELETE FROM FaceCrop WHERE CropUID=?1 AND CameraID=?2" );
		remove.Bind( 1, uid );
		remove.Bind( 2, cameraID );
		remove.Run();
		if( sqlite3_changes( database ) != 1 ) throw std::runtime_error( "eligible face crop vanished before deletion" );
	}

	void ProcessPage( sqlite3* database, int cameraID, int kind, double cutoffEpoch,
		DetectionCleanupStats& stats )
	{
		const Cursor cursor = LoadCursor( database, cameraID, kind );
		const auto page = SelectPage( database, cameraID, kind, cutoffEpoch, cursor );
		if( page.empty() && !cursor.Stored ) return;
		if( std::none_of( page.begin(), page.end(), []( const Candidate& row ){ return row.Eligible; } ) )
		{
			StoreCursor( database, cameraID, kind, page );
			return;
		}

		Transaction transaction( database );
		for( const Candidate& row : page )
		{
			if( !row.Eligible || !StillEligible( database, cameraID, kind, row.UID, cutoffEpoch ) ) continue;
			if( kind == FrameKind )
			{
				DeleteFrame( database, cameraID, row.UID );
				++stats.FramesDeleted;
			}
			else
			{
				DeleteFaceCrop( database, cameraID, row.UID );
				++stats.FaceCropsDeleted;
			}
		}
		StoreCursor( database, cameraID, kind, page );
		transaction.Commit();
	}

	bool PathStillReferenced( sqlite3* database, const std::string& path )
	{
		Statement query( database,
			"SELECT EXISTS(SELECT 1 FROM DetectionFrame WHERE FramePath=?1) "
			"OR EXISTS(SELECT 1 FROM DetectionBox WHERE CropPath=?1) "
			"OR EXISTS(SELECT 1 FROM FaceCrop WHERE FilePath=?1)" );
		query.Bind( 1, path );
		if( !query.Row() ) throw std::runtime_error( "path reference check returned no row" );
		return query.Int( 0 ) != 0;
	}

	void DrainPendingFiles( sqlite3* database, const std::string& cachePath, double cutoffEpoch,
		DetectionCleanupStats& stats )
	{
		const double nowEpoch = static_cast<double>( std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch() ).count() );
		Statement select( database,
			"SELECT CameraID,Path FROM DetectionAssetDeletePending WHERE RetryAfter<=?1 "
			"ORDER BY RetryAfter,Path LIMIT ?2" );
		select.Bind( 1, nowEpoch );
		select.Bind( 2, FileBatchSize );
		std::vector<std::pair<int,std::string>> pending;
		while( select.Row() ) pending.emplace_back( select.Int( 0 ), select.Text( 1 ) );
		auto defer = [&]( const std::string& path, double retryAfter ) {
			Statement update( database, "UPDATE DetectionAssetDeletePending SET RetryAfter=?1 WHERE Path=?2" );
			update.Bind( 1, retryAfter );
			update.Bind( 2, path );
			update.Run();
		};

		for( const auto& [cameraID, path] : pending )
		{
			// A producer writes a JPEG before inserting its row. If a filename
			// has been reused recently, wait for that row to appear rather than
			// unlinking the producer's still-unregistered file.
			std::error_code timeError;
			const auto modified = std::filesystem::last_write_time( path, timeError );
			if( !timeError )
			{
				const auto cutoffSystem = std::chrono::system_clock::time_point(
					std::chrono::seconds( static_cast<int64_t>( cutoffEpoch ) ) );
				const auto elapsed = std::chrono::system_clock::now() - cutoffSystem;
				const auto cutoffTime = std::filesystem::file_time_type::clock::now() -
					std::chrono::duration_cast<std::filesystem::file_time_type::duration>( elapsed );
				if( modified >= cutoffTime )
				{
					defer( path, nowEpoch + 300 );
					continue;
				}
			}
			// Hold the writer reservation only for one local file operation. This
			// prevents a new row from acquiring the same path between check and unlink.
			Transaction transaction( database );
			const bool referenced = PathStillReferenced( database, path );
			if( !referenced && !DeleteManagedDetectionAssets( cachePath, cameraID, { path } ) )
			{
				defer( path, nowEpoch + 30 );
				transaction.Commit();
				continue;
			}
			Statement clear( database, "DELETE FROM DetectionAssetDeletePending WHERE Path=?1 AND CameraID=?2" );
			clear.Bind( 1, path );
			clear.Bind( 2, cameraID );
			clear.Run();
			transaction.Commit();
			if( !referenced ) ++stats.AssetsHandled;
		}
	}
}

DetectionCleanup::DetectionCleanup( sqlite3* Database, std::string CachePath )
	: m_Database( Database ), m_CachePath( std::move( CachePath ) )
{
	if( !m_Database ) throw std::invalid_argument( "DetectionCleanup requires a SQLite connection" );
}

DetectionCleanupStats DetectionCleanup::RunPass( double CutoffEpoch )
{
	Statement cameras( m_Database,
		"SELECT DISTINCT CameraID FROM DetectionFrame WHERE Timestamp<?1 "
		"UNION SELECT DISTINCT CameraID FROM FaceCrop WHERE Timestamp<?1 "
		"UNION SELECT CameraID FROM DetectionCleanupProgress" );
	cameras.Bind( 1, CutoffEpoch );
	std::vector<int> cameraIDs;
	while( cameras.Row() ) cameraIDs.push_back( cameras.Int( 0 ) );

	DetectionCleanupStats stats;
	for( int cameraID : cameraIDs )
	{
		ProcessPage( m_Database, cameraID, FaceCropKind, CutoffEpoch, stats );
		ProcessPage( m_Database, cameraID, FrameKind, CutoffEpoch, stats );
	}
	DrainPendingFiles( m_Database, m_CachePath, CutoffEpoch, stats );
	return stats;
}

void RunDetectionCleanupWorker( std::stop_token Stop, const std::string& DatabasePath,
	const std::string& CachePath, int RetentionDays )
{
	sqlite3* rawDatabase = nullptr;
	const int openResult = sqlite3_open_v2( DatabasePath.c_str(), &rawDatabase,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_FULLMUTEX, nullptr );
	if( openResult != SQLITE_OK )
	{
		LOG_ERROR( "Detection cleanup could not open its private database connection: %s",
			rawDatabase ? sqlite3_errmsg( rawDatabase ) : sqlite3_errstr( openResult ) );
		if( rawDatabase ) sqlite3_close_v2( rawDatabase );
		return;
	}
	std::unique_ptr<sqlite3,decltype( &sqlite3_close_v2 )> database( rawDatabase, sqlite3_close_v2 );
	sqlite3_busy_timeout( database.get(), 1000 );
	try
	{
		Exec( database.get(), "PRAGMA foreign_keys=ON" );
		{
			Statement mode( database.get(), "PRAGMA journal_mode" );
			if( !mode.Row() || mode.Text( 0 ) != "wal" )
			{
				LOG_ERROR( "Detection cleanup needs WAL mode to avoid blocking HTTP reads; cleanup is disabled." );
				return;
			}
		}
		DetectionCleanup cleanup( database.get(), CachePath );
		while( !Stop.stop_requested() )
		{
			try
			{
				const auto now = std::chrono::system_clock::now();
				const auto cutoff = now - std::chrono::hours( 24 * RetentionDays );
				const double cutoffEpoch = static_cast<double>(
					std::chrono::duration_cast<std::chrono::seconds>( cutoff.time_since_epoch() ).count() );
				const auto stats = cleanup.RunPass( cutoffEpoch );
				if( stats.FramesDeleted || stats.FaceCropsDeleted || stats.AssetsHandled )
					LOG_INFO( "Detection cleanup: %d frames, %d face crops, %d asset paths handled.",
						stats.FramesDeleted, stats.FaceCropsDeleted, stats.AssetsHandled );
			}
			catch( const std::exception& error )
			{
				LOG_WARNING( "Detection cleanup pass will retry: %s", error.what() );
			}
			for( int second = 0; second < 30 && !Stop.stop_requested(); ++second )
				std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
		}
	}
	catch( const std::exception& error )
	{
		LOG_ERROR( "Detection cleanup worker stopped: %s", error.what() );
	}
}
