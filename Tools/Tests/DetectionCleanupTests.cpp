#include "DetectionCleanup.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace
{
	void Require( bool condition, const char* message )
	{
		if( !condition ) throw std::runtime_error( message );
	}

	void Exec( sqlite3* database, const char* sql )
	{
		char* error = nullptr;
		const int result = sqlite3_exec( database, sql, nullptr, nullptr, &error );
		if( result != SQLITE_OK )
		{
			const std::string message = error ? error : sqlite3_errmsg( database );
			sqlite3_free( error );
			throw std::runtime_error( message );
		}
	}

	class Insert
	{
	public:
		Insert( sqlite3* database, const char* sql ) : m_Database( database )
		{
			if( sqlite3_prepare_v2( database, sql, -1, &m_Statement, nullptr ) != SQLITE_OK )
				throw std::runtime_error( sqlite3_errmsg( database ) );
		}
		~Insert() { sqlite3_finalize( m_Statement ); }
		void Int( int index, int64_t value ) { sqlite3_bind_int64( m_Statement, index, value ); }
		void Double( int index, double value ) { sqlite3_bind_double( m_Statement, index, value ); }
		void Text( int index, const fs::path& value )
		{
			const std::string path = value.string();
			sqlite3_bind_text( m_Statement, index, path.c_str(), -1, SQLITE_TRANSIENT );
		}
		void Run()
		{
			if( sqlite3_step( m_Statement ) != SQLITE_DONE )
				throw std::runtime_error( sqlite3_errmsg( m_Database ) );
		}
	private:
		sqlite3* m_Database;
		sqlite3_stmt* m_Statement = nullptr;
	};

	int Count( sqlite3* database, const char* sql )
	{
		sqlite3_stmt* statement = nullptr;
		if( sqlite3_prepare_v2( database, sql, -1, &statement, nullptr ) != SQLITE_OK )
			throw std::runtime_error( sqlite3_errmsg( database ) );
		const int result = sqlite3_step( statement );
		if( result != SQLITE_ROW )
		{
			sqlite3_finalize( statement );
			throw std::runtime_error( sqlite3_errmsg( database ) );
		}
		const int count = sqlite3_column_int( statement, 0 );
		sqlite3_finalize( statement );
		return count;
	}

	void MakeFile( const fs::path& path )
	{
		fs::create_directories( path.parent_path() );
		std::ofstream file( path, std::ios::binary );
		file << 'x';
	}

	struct TestCache
	{
		fs::path Root = fs::temp_directory_path() /
			( "witness-cleanup-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
		~TestCache() { std::error_code error; fs::remove_all( Root, error ); }
	};
}

int main()
{
	try
	{
		TestCache cache;
		sqlite3* raw = nullptr;
		Require( sqlite3_open( ":memory:", &raw ) == SQLITE_OK, "could not open in-memory database" );
		std::unique_ptr<sqlite3,decltype( &sqlite3_close )> database( raw, sqlite3_close );
		Exec( raw, R"SQL(
			PRAGMA foreign_keys=ON;
			CREATE TABLE DetectionFrame(FrameUID INTEGER PRIMARY KEY,CameraID INTEGER,Timestamp REAL,FramePath TEXT);
			CREATE INDEX idx_detframe_camera_time ON DetectionFrame(CameraID,Timestamp);
			CREATE INDEX idx_detframe_framepath ON DetectionFrame(FramePath) WHERE FramePath IS NOT NULL;
			CREATE TABLE DetectionBox(BoxUID INTEGER PRIMARY KEY,FrameUID INTEGER,CropPath TEXT,
				FOREIGN KEY(FrameUID) REFERENCES DetectionFrame(FrameUID) ON DELETE CASCADE);
			CREATE INDEX idx_detbox_croppath ON DetectionBox(CropPath) WHERE CropPath IS NOT NULL;
			CREATE TABLE FaceCrop(CropUID INTEGER PRIMARY KEY,CameraID INTEGER,Timestamp REAL,FrameUID INTEGER,FilePath TEXT,
				FOREIGN KEY(FrameUID) REFERENCES DetectionFrame(FrameUID) ON DELETE CASCADE);
			CREATE INDEX idx_facecrop_camera_time ON FaceCrop(CameraID,Timestamp);
			CREATE INDEX idx_facecrop_filepath ON FaceCrop(FilePath);
			CREATE TABLE FaceEmbedding(EmbeddingUID INTEGER PRIMARY KEY,FaceCropUID INTEGER,Verified INTEGER);
			CREATE INDEX idx_embedding_crop ON FaceEmbedding(FaceCropUID);
			CREATE TABLE Clip(ClipUID INTEGER PRIMARY KEY,Camera INTEGER,Timestamp REAL,Duration INTEGER);
			CREATE INDEX idx_clip_camera_ts ON Clip(Camera,Timestamp DESC);
			CREATE TABLE ContinuousSegment(SegmentUID INTEGER PRIMARY KEY,CameraUID INTEGER,StartTimestamp REAL,EndTimestamp REAL);
			CREATE INDEX ContinuousSegmentByCameraTime ON ContinuousSegment(CameraUID,StartTimestamp);
			CREATE TABLE DetectionCleanupProgress(CameraID INTEGER,Kind INTEGER,CursorTimestamp REAL,CursorUID INTEGER,
				PRIMARY KEY(CameraID,Kind));
			CREATE TABLE DetectionAssetDeletePending(Path TEXT PRIMARY KEY,CameraID INTEGER,RetryAfter REAL NOT NULL DEFAULT 0);
		)SQL" );

		// The fake clock is thirty days ahead. The retention cutoff is three days
		// before that, so the exact-boundary and newer rows must survive.
		const double fakeNow = static_cast<double>( std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now() ) ) + 30.0 * 86400.0;
		const double cutoff = fakeNow - 3.0 * 86400.0;
		const fs::path oldFrame = cache.Root / "frames/1/old.jpg";
		const fs::path oldBox = cache.Root / "crops/1/old.jpg";
		const fs::path oldFace = cache.Root / "faces/1/old.jpg";
		const fs::path verifiedFace = cache.Root / "faces/1/verified.jpg";
		const fs::path sharedFace = cache.Root / "faces/1/shared.jpg";
		const fs::path protectedFrame = cache.Root / "frames/1/protected.jpg";
		const fs::path segmentFrame = cache.Root / "frames/1/segment.jpg";
		const fs::path boundaryFrame = cache.Root / "frames/1/boundary.jpg";
		const fs::path newerFrame = cache.Root / "frames/1/newer.jpg";
		const fs::path wrongCamera = cache.Root / "frames/2/wrong.jpg";
		for( const fs::path& path : { oldFrame, oldBox, oldFace, verifiedFace, sharedFace,
			protectedFrame, segmentFrame, boundaryFrame, newerFrame, wrongCamera } ) MakeFile( path );

		auto addFrame = [&]( int64_t uid, int camera, double timestamp, const fs::path& path ) {
			Insert row( raw, "INSERT INTO DetectionFrame VALUES(?1,?2,?3,?4)" );
			row.Int( 1, uid ); row.Int( 2, camera ); row.Double( 3, timestamp ); row.Text( 4, path ); row.Run();
		};
		addFrame( 1, 1, cutoff - 40, oldFrame );
		addFrame( 2, 1, cutoff - 30, protectedFrame );
		addFrame( 3, 1, cutoff - 20, segmentFrame );
		addFrame( 4, 1, cutoff, boundaryFrame );
		addFrame( 5, 1, cutoff + 1, newerFrame );
		addFrame( 6, 1, cutoff - 10, wrongCamera );
		{
			Insert row( raw, "INSERT INTO DetectionBox VALUES(1,1,?1)" ); row.Text( 1, oldBox ); row.Run();
		}
		{
			Insert row( raw, "INSERT INTO Clip VALUES(1,1,?1,2)" ); row.Double( 1, cutoff - 31 ); row.Run();
			Insert segment( raw, "INSERT INTO ContinuousSegment VALUES(1,1,?1,?2)" );
			segment.Double( 1, cutoff - 21 ); segment.Double( 2, cutoff - 19 ); segment.Run();
		}
		auto addCrop = [&]( int64_t uid, double timestamp, int64_t frame, const fs::path& path ) {
			Insert row( raw, "INSERT INTO FaceCrop VALUES(?1,1,?2,?3,?4)" );
			row.Int( 1, uid ); row.Double( 2, timestamp ); row.Int( 3, frame ); row.Text( 4, path ); row.Run();
		};
		addCrop( 1, cutoff - 40, 1, oldFace );
		addCrop( 2, cutoff - 40, 1, verifiedFace );
		addCrop( 3, cutoff - 40, 1, sharedFace );
		addCrop( 4, cutoff - 30, 2, sharedFace );
		addCrop( 5, cutoff + 1, 5, newerFrame );
		Exec( raw, "INSERT INTO FaceEmbedding VALUES(1,2,1)" );

		// A full candidate page with no eligible rows must not trap the cursor.
		for( int id = 1000; id < 1100; ++id ) addFrame( id, 2, cutoff - 1000 + id - 1000, {} );
		addFrame( 1100, 2, cutoff - 800, {} );
		{
			Insert row( raw, "INSERT INTO Clip VALUES(2,2,?1,101)" );
			row.Double( 1, cutoff - 1001 ); row.Run();
		}

		DetectionCleanup cleanup( raw, cache.Root.string() );
		const auto first = cleanup.RunPass( cutoff );
		Require( first.FramesDeleted == 2, "first pass deleted the wrong frames" );
		Require( first.FaceCropsDeleted == 2, "first pass deleted the wrong face crops" );
		Require( Count( raw, "SELECT COUNT(*) FROM DetectionFrame WHERE FrameUID IN (1,6)" ) == 0, "eligible old frames survived" );
		Require( Count( raw, "SELECT COUNT(*) FROM DetectionFrame WHERE FrameUID IN (2,3,4,5,1100)" ) == 5,
			"protected, boundary, newer, or deferred frame was deleted" );
		Require( Count( raw, "SELECT COUNT(*) FROM FaceCrop WHERE CropUID IN (1,3)" ) == 0, "eligible face crops survived" );
		Require( Count( raw, "SELECT COUNT(*) FROM FaceCrop WHERE CropUID IN (2,4,5)" ) == 3,
			"protected or newer face crop was deleted" );
		Require( Count( raw, "SELECT COUNT(*) FROM FaceCrop WHERE CropUID=2 AND FrameUID IS NULL" ) == 1,
			"verified face crop was not detached before frame deletion" );
		Require( !fs::exists( oldFrame ) && !fs::exists( oldBox ) && !fs::exists( oldFace ),
			"expired assets were not removed" );
		Require( fs::exists( verifiedFace ) && fs::exists( sharedFace ) && fs::exists( protectedFrame ) &&
			fs::exists( segmentFrame ) && fs::exists( boundaryFrame ) && fs::exists( newerFrame ) &&
			fs::exists( wrongCamera ), "a protected, future, or cross-camera asset was removed" );
		Require( Count( raw, "SELECT COUNT(*) FROM DetectionAssetDeletePending" ) == 0, "pending asset queue did not drain" );

		// A new cleanup instance must resume from the persisted cursor.
		DetectionCleanup restarted( raw, cache.Root.string() );
		const auto second = restarted.RunPass( cutoff );
		Require( second.FramesDeleted == 1, "persisted cursor did not reach the later eligible frame" );
		Require( Count( raw, "SELECT COUNT(*) FROM DetectionFrame WHERE FrameUID=1100" ) == 0,
			"eligible frame after protected page survived" );
		Require( Count( raw, "SELECT COUNT(*) FROM DetectionFrame WHERE FrameUID BETWEEN 1000 AND 1099" ) == 100,
			"protected page was deleted" );

		// A path freshly rewritten under an old filename is not unlinked until
		// its filesystem timestamp also falls outside the retention window.
		const fs::path reused = cache.Root / "frames/1/reused.jpg";
		MakeFile( reused );
		{
			Insert pending( raw, "INSERT INTO DetectionAssetDeletePending(Path,CameraID) VALUES(?1,1)" );
			pending.Text( 1, reused ); pending.Run();
		}
		const double realNow = static_cast<double>( std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now() ) );
		restarted.RunPass( realNow - 3.0 * 86400.0 );
		Require( fs::exists( reused ) && Count( raw, "SELECT COUNT(*) FROM DetectionAssetDeletePending" ) == 1,
			"recently rewritten asset was removed" );
		fs::last_write_time( reused, fs::file_time_type::clock::now() - std::chrono::hours( 24 * 4 ) );
		Exec( raw, "UPDATE DetectionAssetDeletePending SET RetryAfter=0" ); // simulate retry time elapsing
		restarted.RunPass( realNow - 3.0 * 86400.0 );
		Require( !fs::exists( reused ) && Count( raw, "SELECT COUNT(*) FROM DetectionAssetDeletePending" ) == 0,
			"old pending asset did not drain" );
		std::cout << "DetectionCleanup future-cutoff safety tests passed\n";
		return 0;
	}
	catch( const std::exception& error )
	{
		std::cerr << "DetectionCleanup test failed: " << error.what() << '\n';
		return 1;
	}
}
