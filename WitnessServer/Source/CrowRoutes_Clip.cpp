#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ===== Clip Handlers =====

static std::string GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video )
{
	std::stringstream Stream;
	Stream << CameraID;

	if( Video )
	{
		if( Manual )
			Stream << "_Manual";
		else
			Stream << "_Auto";
	}

	Stream << "_" << Timestamp;

	if( Video )
		Stream << ".mp4";
	else
		Stream << ".jpg";

	return (fs::path( Context.CachePath ) / Stream.str()).string();
}

void CrowListener::HandleClipThumbnail( const crow::request& req, crow::response& res, int cameraId, const std::string& clipId, bool video )
{
	uint64_t TargetCameraTimestamp = std::stoull( clipId );

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}
	
	// Try in-memory thumbnail first (non-video only)
	if( !video )
	{
		auto CameraState = m_GlobalContext->FindCameraById( cameraId );
		if( CameraState )
		{
			const auto& Camera = CameraState->ClipThumbnails;
			auto IterClip = Camera.find( TargetCameraTimestamp );
			if( IterClip != Camera.end() && (*IterClip).second.size() != 0 )
			{
				res.set_header( "Content-Type", "image/jpeg" );
				res.set_header( "Cache-Control", "no-cache, no-store, must-revalidate" );
				res.body.assign( (const char*)(*IterClip).second.data(), (*IterClip).second.size() );
				res.code = 200;
				res.end();
				return;
			}
		}
	}

	// Look up from database
	SQLiteDatabaseQueryInstance SelectClip( m_GlobalContext->Database, "SelectClip" );
	SelectClip->Bind( "@CameraID", cameraId );
	SelectClip->Bind( "@Timestamp", (int64_t)TargetCameraTimestamp );

	std::string ClipFilename;
	bool Success = false;

	SelectClip->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			int64_t Timestamp = query.GetColumnValueInt64( 1 );
			int CameraID = query.GetColumnValueInt( 2 );
			int RecordMode = query.GetColumnValueInt( 6 );

			ClipFilename = GetClipName( *m_GlobalContext, CameraID, Timestamp, RecordMode == 0, video );
			Success = true;
			return true;
		}
	);

	if( Success && fs::exists( ClipFilename ) )
	{
		std::ifstream file( ClipFilename, std::ios::binary );
		if( file )
		{
			std::string body( (std::istreambuf_iterator<char>(file)),
				std::istreambuf_iterator<char>() );

			res.set_header( "Content-Type", video ? "video/mp4" : "image/jpeg" );
			res.body = std::move( body );
			res.code = 200;
			res.end();
			return;
		}
	}

	res.code = 404;
	res.end();
}

void CrowListener::HandleClipEnum( const crow::request& req, crow::response& res, int cameraId, int maxCount, const std::string& startDate, const std::string& rangePeriod, int pageOffset )
{
	const int MaxClipsPerQuery = 100;
	uint64_t StartDateInt = std::stoull( startDate );
	uint64_t RangePeriodInt = std::stoull( rangePeriod );
	maxCount = std::min( maxCount, MaxClipsPerQuery );

	int UserUID = 0;
	if( cameraId == -1 )
	{
		UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
			CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
		if( UserUID < 0 )
		{
			res.code = 400;
			res.end();
			return;
		}
	}
	else
	{
		UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
			CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
		if( UserUID <= 0 )
		{
			res.code = 403;
			res.end();
			return;
		}
	}

	// Read optional filter query params
	const char* pReviewed = req.url_params.get( "reviewed" );
	const char* pSaved = req.url_params.get( "saved" );
	const char* pMode = req.url_params.get( "mode" );
	const char* pLighting = req.url_params.get( "lighting" );
	const char* pMinDuration = req.url_params.get( "minDuration" );
	const char* pTags = req.url_params.get( "tags" );

	bool hasFilters = pReviewed || pSaved || pMode || pLighting || pMinDuration || pTags;

	int Count = 0;
	std::vector<crow::json::wvalue> Array;
	std::vector<int64_t> clipUIDs;

	if( hasFilters )
	{
		// Build dynamic SQL with filters
		std::string extraWhere;

		if( pReviewed )
			extraWhere += " AND Clip.Reviewed = " + std::string( pReviewed );
		if( pSaved )
			extraWhere += " AND Clip.Save = " + std::string( pSaved );
		if( pMode )
			extraWhere += " AND Clip.RecordMode = " + std::string( pMode );
		if( pLighting )
			extraWhere += " AND Clip.Lighting = " + std::string( pLighting );
		if( pMinDuration )
			extraWhere += " AND Clip.Duration >= " + std::string( pMinDuration );

		// Parse and validate tag names
		std::vector<std::string> tagNames;
		if( pTags )
		{
			std::istringstream tagStream( pTags );
			std::string tagName;
			while( std::getline( tagStream, tagName, ',' ) )
			{
				if( tagName.empty() ) continue;
				// Reject unsafe characters
				bool safe = true;
				for( char c : tagName )
				{
					if( c == '\'' || c == '"' || c == ';' || c == '\\' || c == '-' )
					{
						safe = false;
						break;
					}
				}
				if( safe )
					tagNames.push_back( tagName );
			}

			// Tag filter: OR — match clips having ANY of the specified tags
			if( !tagNames.empty() )
			{
				std::string inList;
				for( size_t t = 0; t < tagNames.size(); t++ )
				{
					if( t > 0 ) inList += ",";
					inList += "'" + tagNames[t] + "'";
				}
				extraWhere += " AND Clip.ClipUID IN ("
					"SELECT ct.ClipUID FROM ClipTag ct"
					" INNER JOIN Tag t ON t.TagUID = ct.TagUID"
					" WHERE t.Name IN (" + inList + "))";
			}
		}

		std::string tsFrom = std::to_string( (int64_t)(StartDateInt - RangePeriodInt) );
		std::string tsTo = std::to_string( (int64_t)StartDateInt );

		std::string countSQL, selectSQL;
		if( cameraId == -1 )
		{
			countSQL = "SELECT COUNT(DISTINCT Clip.ClipUID) FROM Clip"
				" INNER JOIN Camera C ON C.CameraUID = Clip.Camera"
				" INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID"
				" INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`"
				" WHERE Clip.Timestamp >= " + tsFrom + " AND Clip.Timestamp <= " + tsTo +
				" AND UGM.UserUID = " + std::to_string( UserUID ) + extraWhere;

			selectSQL = "SELECT DISTINCT Clip.* FROM Clip"
				" INNER JOIN Camera C ON C.CameraUID = Clip.Camera"
				" INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID"
				" INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`"
				" WHERE Clip.Timestamp >= " + tsFrom + " AND Clip.Timestamp <= " + tsTo +
				" AND UGM.UserUID = " + std::to_string( UserUID ) + extraWhere +
				" ORDER BY Clip.Timestamp DESC LIMIT " + std::to_string( maxCount ) + " OFFSET " + std::to_string( pageOffset );
		}
		else
		{
			countSQL = "SELECT COUNT(DISTINCT Clip.ClipUID) FROM Clip"
				" WHERE Clip.Camera = " + std::to_string( cameraId ) +
				" AND Clip.Timestamp >= " + tsFrom + " AND Clip.Timestamp <= " + tsTo + extraWhere;

			selectSQL = "SELECT Clip.* FROM Clip"
				" WHERE Clip.Camera = " + std::to_string( cameraId ) +
				" AND Clip.Timestamp >= " + tsFrom + " AND Clip.Timestamp <= " + tsTo + extraWhere +
				" ORDER BY Clip.Timestamp DESC LIMIT " + std::to_string( maxCount ) + " OFFSET " + std::to_string( pageOffset );
		}

		// Execute count query
		sqlite3_exec( m_GlobalContext->Database->GetDatabase(), countSQL.c_str(),
			[]( void* data, int, char** argv, char** ) -> int
			{
				if( argv[0] )
					*static_cast<int*>( data ) = std::atoi( argv[0] );
				return 0;
			}, &Count, nullptr );

		// Execute select query
		struct EnumContext { std::vector<crow::json::wvalue>* array; std::vector<int64_t>* clipUIDs; };
		EnumContext ctx{ &Array, &clipUIDs };

		if( Count > 0 )
		{
			sqlite3_exec( m_GlobalContext->Database->GetDatabase(), selectSQL.c_str(),
				[]( void* data, int, char** argv, char** ) -> int
				{
					auto* ctx = static_cast<EnumContext*>( data );
					if( !argv[0] ) return 0;

					int64_t ClipID = std::stoll( argv[0] );
					uint64_t Timestamp = argv[1] ? std::stoull( argv[1] ) : 0;
					int CameraID = argv[2] ? std::atoi( argv[2] ) : 0;
					uint64_t MotionTimestamp = argv[3] ? std::stoull( argv[3] ) : 0;
					int ActiveDuration = argv[4] ? std::atoi( argv[4] ) : 0;
					int Duration = argv[5] ? std::atoi( argv[5] ) : 0;
					int RecordMode = argv[6] ? std::atoi( argv[6] ) : 0;
					double MaxMotion = argv[7] ? std::atof( argv[7] ) : 0.0;
					std::string Description = argv[8] ? argv[8] : "";
					int Saved = argv[9] ? std::atoi( argv[9] ) : 0;
					int Lighting = argv[12] ? std::atoi( argv[12] ) : 0;
					int Reviewed = argv[13] ? std::atoi( argv[13] ) : 0;

					crow::json::wvalue Clip;
					Clip["clipUID"] = ClipID;
					Clip["timestamp"] = Timestamp;
					Clip["cameraID"] = CameraID;
					Clip["motionTimestamp"] = MotionTimestamp;
					Clip["activeDuration"] = ActiveDuration;
					Clip["duration"] = Duration;
					Clip["recordMode"] = RecordMode;
					Clip["maxMotion"] = MaxMotion;
					Clip["description"] = Description;
					Clip["saved"] = Saved;
					Clip["lighting"] = Lighting;
					Clip["reviewed"] = Reviewed;

					ctx->clipUIDs->push_back( ClipID );
					ctx->array->push_back( std::move( Clip ) );
					return 0;
				}, &ctx, nullptr );
		}
	}
	else
	{
	// Unfiltered path: use prepared statements
	{
		SQLiteDatabaseQueryInstance CountClips( m_GlobalContext->Database, cameraId == -1 ? "CountClipsWithinRangeAll" : "CountClipsWithinRange" );
		if( cameraId == -1 )
			CountClips->Bind( "@UserUID", UserUID );
		else
			CountClips->Bind( "@CameraID", cameraId );
		CountClips->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		CountClips->Bind( "@TimestampTo", (int64_t)StartDateInt );
		CountClips->Bind( "@MaxCount", maxCount );
		CountClips->Bind( "@PageOffset", pageOffset );

		CountClips->Execute(
			[&Count]( const SQLiteDatabaseQuery& query )
			{
				Count = query.GetColumnValueInt( 0 );
				return true;
			}
		);
	}

	if( Count > 0 )
	{
		SQLiteDatabaseQueryInstance SelectClips( m_GlobalContext->Database, cameraId == -1 ? "SelectClipsWithinRangeAll" : "SelectClipsWithinRange" );
		if( cameraId == -1 )
			SelectClips->Bind( "@UserUID", UserUID );
		else
			SelectClips->Bind( "@CameraID", cameraId );
		SelectClips->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		SelectClips->Bind( "@TimestampTo", (int64_t)StartDateInt );
		SelectClips->Bind( "@MaxCount", maxCount );
		SelectClips->Bind( "@PageOffset", pageOffset );

		SelectClips->Execute(
			[&Array, &clipUIDs]( const SQLiteDatabaseQuery& query )
			{
				int64_t ClipID = query.GetColumnValueInt64( 0 );
				uint64_t Timestamp = query.GetColumnValueInt64( 1 );
				int CameraID = query.GetColumnValueInt( 2 );
				uint64_t MotionTimestamp = query.GetColumnValueInt64( 3 );
				int ActiveDuration = query.GetColumnValueInt( 4 );
				int Duration = query.GetColumnValueInt( 5 );
				int RecordMode = query.GetColumnValueInt( 6 );
				double MaxMotion = query.GetColumnValueDouble( 7 );

				const char* DescriptionStr = query.GetColumnValueText( 8 );
				std::string Description = DescriptionStr ? DescriptionStr : "";

				int Saved = query.GetColumnValueInt( 9 );
				int Lighting = query.GetColumnValueInt( 12 );
				int Reviewed = query.GetColumnValueInt( 13 );

				crow::json::wvalue Clip;
				Clip["clipUID"] = ClipID;
				Clip["timestamp"] = Timestamp;
				Clip["cameraID"] = CameraID;
				Clip["motionTimestamp"] = MotionTimestamp;
				Clip["activeDuration"] = ActiveDuration;
				Clip["duration"] = Duration;
				Clip["recordMode"] = RecordMode;
				Clip["maxMotion"] = MaxMotion;
				Clip["description"] = Description;
				Clip["saved"] = Saved;
				Clip["lighting"] = Lighting;
				Clip["reviewed"] = Reviewed;

				clipUIDs.push_back( ClipID );
				Array.push_back( std::move( Clip ) );
				return true;
			}
		);
	}
	} // end unfiltered path

	// Populate tags from ClipTag junction table
	for( size_t i = 0; i < clipUIDs.size(); i++ )
	{
		std::string tags;
		{
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectTagsForClip" );
			q->Bind( "@ClipUID", clipUIDs[i] );
			q->Execute( [&tags]( const SQLiteDatabaseQuery& query )
			{
				const char* name = query.GetColumnValueText( 1 );
				if( name )
				{
					if( !tags.empty() ) tags += ";";
					tags += name;
				}
				return true;
			});
		}
		Array[i]["tags"] = tags;
	}

	crow::json::wvalue Data;
	Data["count"] = Count;
	Data["clips"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipToggleSave( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body || !body.has("id") || !body.has("value") )
	{
		res.code = 400;
		res.end();
		return;
	}

	int ClipUID = (int)body["id"].i();
	bool Value = body["value"].b();

	// Get camera ID for auth check
	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, "SelectClipID" );
		SelectClipID->Bind( "@ClipUID", ClipUID );
		SelectClipID->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt( 2 );
				return true;
			}
		);
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, TargetCameraInt );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	SQLiteDatabaseQueryInstance SetClipSaveState( m_GlobalContext->Database, "SetClipSaveState" );
	SetClipSaveState->Bind( "@ClipUID", ClipUID );
	SetClipSaveState->Bind( "@Save", Value ? 1 : 0 );
	SetClipSaveState->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body || !body.has("id") )
	{
		res.code = 400;
		res.end();
		return;
	}

	int ClipUID = (int)body["id"].i();

	// Get camera ID for auth check
	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, "SelectClipID" );
		SelectClipID->Bind( "@ClipUID", ClipUID );
		SelectClipID->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt( 2 );
				return true;
			}
		);
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, TargetCameraInt );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int CameraID = 0;
	int64_t Timestamp = 0;
	bool Manual = false;

	SQLiteDatabaseQueryInstance FindClipByUID( m_GlobalContext->Database, "FindClipByUID" );
	FindClipByUID->Bind( "@ClipUID", ClipUID );
	FindClipByUID->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			Timestamp = query.GetColumnValueInt64( 1 );
			CameraID = query.GetColumnValueInt( 2 );
			Manual = query.GetColumnValueInt( 6 ) == 0;
			return true;
		}
	);

	// Delete files
	auto ThumbnailPath = GetClipName( *m_GlobalContext, CameraID, Timestamp, Manual, false );
	auto VideoPath = GetClipName( *m_GlobalContext, CameraID, Timestamp, Manual, true );

	std::error_code error;
	std::filesystem::remove( ThumbnailPath, error );
	std::filesystem::remove( VideoPath, error );

	SQLiteDatabaseQueryInstance DeleteClipQuery( m_GlobalContext->Database, "DeleteClip" );
	DeleteClipQuery->Bind( "@ClipUID", ClipUID );
	DeleteClipQuery->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipRetag( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body || !body.has("id") )
	{
		res.code = 400;
		res.end();
		return;
	}

	int ClipUID = (int)body["id"].i();

	// Get camera ID for auth check
	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, "SelectClipID" );
		SelectClipID->Bind( "@ClipUID", ClipUID );
		SelectClipID->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt( 2 );
				return true;
			}
		);
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, TargetCameraInt );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	SQLiteDatabaseQueryInstance ResetClipDetection( m_GlobalContext->Database, "ResetClipDetection" );
	ResetClipDetection->Bind( "@ClipUID", ClipUID );
	ResetClipDetection->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}