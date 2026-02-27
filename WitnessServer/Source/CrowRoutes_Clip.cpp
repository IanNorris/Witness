#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ===== Clip Handlers =====

static StringT GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video )
{
	StringStreamT Stream;
	Stream << CameraID;

	if( Video )
	{
		if( Manual )
			Stream << _T("_Manual");
		else
			Stream << _T("_Auto");
	}

	Stream << _T("_") << Timestamp;

	if( Video )
		Stream << _T(".mp4");
	else
		Stream << _T(".jpg");

	return (fs::path( Context.CachePath ) / Stream.str()).native();
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
	SQLiteDatabaseQueryInstance SelectClip( m_GlobalContext->Database, _T("SelectClip") );
	SelectClip->Bind( "@CameraID", cameraId );
	SelectClip->Bind( "@Timestamp", (int64_t)TargetCameraTimestamp );

	StringT ClipFilename;
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

	int Count = 0;
	std::vector<crow::json::wvalue> Array;

	{
		SQLiteDatabaseQueryInstance CountClips( m_GlobalContext->Database, cameraId == -1 ? _T("CountClipsWithinRangeAll") : _T("CountClipsWithinRange") );
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
		SQLiteDatabaseQueryInstance SelectClips( m_GlobalContext->Database, cameraId == -1 ? _T("SelectClipsWithinRangeAll") : _T("SelectClipsWithinRange") );
		if( cameraId == -1 )
			SelectClips->Bind( "@UserUID", UserUID );
		else
			SelectClips->Bind( "@CameraID", cameraId );
		SelectClips->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		SelectClips->Bind( "@TimestampTo", (int64_t)StartDateInt );
		SelectClips->Bind( "@MaxCount", maxCount );
		SelectClips->Bind( "@PageOffset", pageOffset );

		SelectClips->Execute(
			[&Array]( const SQLiteDatabaseQuery& query )
			{
				uint64_t ClipID = query.GetColumnValueInt64( 0 );
				uint64_t Timestamp = query.GetColumnValueInt64( 1 );
				int CameraID = query.GetColumnValueInt( 2 );
				uint64_t MotionTimestamp = query.GetColumnValueInt64( 3 );
				int ActiveDuration = query.GetColumnValueInt( 4 );
				int Duration = query.GetColumnValueInt( 5 );
				int RecordMode = query.GetColumnValueInt( 6 );
				double MaxMotion = query.GetColumnValueDouble( 7 );

				const wchar_t* DescriptionStr = query.GetColumnValueText( 8 );
				std::string Description = DescriptionStr ? StringToAnsi( DescriptionStr ) : "";

				int Saved = query.GetColumnValueInt( 9 );

				const wchar_t* TagsStr = query.GetColumnValueText( 10 );
				std::string Tags = TagsStr ? StringToAnsi( TagsStr ) : "";

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
				Clip["tags"] = Tags;

				Array.push_back( std::move( Clip ) );
				return true;
			}
		);
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
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, _T("SelectClipID") );
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

	SQLiteDatabaseQueryInstance SetClipSaveState( m_GlobalContext->Database, _T("SetClipSaveState") );
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
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, _T("SelectClipID") );
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

	SQLiteDatabaseQueryInstance FindClipByUID( m_GlobalContext->Database, _T("FindClipByUID") );
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

	SQLiteDatabaseQueryInstance DeleteClipQuery( m_GlobalContext->Database, _T("DeleteClip") );
	DeleteClipQuery->Bind( "@ClipUID", ClipUID );
	DeleteClipQuery->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}