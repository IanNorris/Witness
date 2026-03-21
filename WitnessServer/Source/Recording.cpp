#include "Common.h"
#include "Database.h"
#include "ClipHelpers.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"
#include "TagHelpers.h"

#include <Log.h>
#include <filesystem>

void WitnessServer::StartCameraRecording( const std::shared_ptr<CameraWorker>& Worker, uint64_t Timestamp, int CameraID, bool IsManual, const ClassificationResult& Result )
{
	bool First = true;
	std::string TagsA = "";
	for( auto& Tag : Result.Tags )
	{
		if( First )
		{
			First = false;
		}
		else
		{
			TagsA += ";";
		}

		TagsA += Tag;
	}
	std::string Tags( TagsA.begin(), TagsA.end() );

	try
	{
		std::filesystem::create_directories( std::filesystem::path(CachePath) );
	}
	catch( const std::filesystem::filesystem_error& e )
	{
		LOG_ERROR( "Failed to create cache directory '%s': %s", CachePath.c_str(), e.what() );
		return;
	}

	std::stringstream TargetFilename;
	TargetFilename << CameraID << (IsManual ? "_Manual_" : "_Auto_") << Timestamp << ".mp4";
	auto TargetPath = (std::filesystem::path(CachePath) / TargetFilename.str()).string();

	auto StartRecord = std::make_shared<CameraStartRecordMessage>( CameraID, Timestamp, TargetPath );
			
	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StartRecord );
	}

	SQLiteDatabaseQueryInstance CreateClip( Context->Database, "CreateClip" );
	CreateClip->Bind( "@Timestamp", (int64_t)Timestamp );
	CreateClip->Bind( "@Camera", CameraID );
	CreateClip->Bind( "@ActiveDuration", 0 );
	CreateClip->Bind( "@Duration", 0 );
	CreateClip->Bind( "@RecordMode", IsManual ? 0 : 1 );
	CreateClip->Bind( "@MaxMotion", 0.0f );
	CreateClip->Bind( "@Description", "" );
	CreateClip->Bind( "@Save", 0 );
	CreateClip->Bind( "@Tags", Tags.c_str() );
	CreateClip->Execute( nullptr );

	// Sync ClipTag junction table
	int64_t clipUID = CreateClip->GetLastInsertionId();
	if( clipUID > 0 )
		TagHelpers::SyncClipTags( Context->Database, clipUID, Tags );
}

void WitnessServer::StopCameraRecording( const ClipStatistics& ClipStats, int CameraID, const ClassificationResult& Result )
{
	bool First = true;
	std::string TagsA = "";
	for( auto& Tag : Result.Tags )
	{
		if( First )
		{
			First = false;
		}
		else
		{
			TagsA += ";";
		}

		TagsA += Tag;
	}
	std::string Tags( TagsA.begin(), TagsA.end() );

	SQLiteDatabaseQueryInstance UpdateClip( Context->Database, "UpdateClip" );
	UpdateClip->Bind( "@Timestamp", (int64_t)ClipStats.TimestampClipStarted );
	UpdateClip->Bind( "@Camera", CameraID );
	UpdateClip->Bind( "@Tags", Tags.c_str() );

	if( ClipStats.TimestampMotionStarted != INT64_MAX )
	{
		UpdateClip->Bind( "@MotionTimestamp", (int64_t)ClipStats.TimestampMotionStarted );
		UpdateClip->Bind( "@ActiveDuration", (int64_t)(ClipStats.TimestampMotionEnded - ClipStats.TimestampMotionStarted) );
	}
	else
	{
		UpdateClip->Bind( "@MotionTimestamp", 0 );
		UpdateClip->Bind( "@ActiveDuration", 0 );
	}

	UpdateClip->Bind( "@Duration", (int64_t)(ClipStats.TimestampClipEnded - ClipStats.TimestampClipStarted) );

	UpdateClip->Bind( "@MaxMotion", ClipStats.LargestMotionDelta );
	UpdateClip->Execute( nullptr );

	// Sync ClipTag junction table — look up ClipUID by timestamp+camera
	{
		SQLiteDatabaseQueryInstance SelectClip( Context->Database, "SelectClip" );
		SelectClip->Bind( "@CameraID", CameraID );
		SelectClip->Bind( "@Timestamp", (int64_t)ClipStats.TimestampClipStarted );
		SelectClip->Execute( [&]( const SQLiteDatabaseQuery& query )
		{
			int64_t clipUID = query.GetColumnValueInt64( 0 );
			TagHelpers::SyncClipTags( Context->Database, clipUID, Tags );

			// Broadcast clip:new event
			crow::json::wvalue ev;
			ev["clipUID"] = clipUID;
			ev["cameraID"] = CameraID;
			ev["timestamp"] = (int64_t)ClipStats.TimestampClipStarted;
			ev["duration"] = (int64_t)(ClipStats.TimestampClipEnded - ClipStats.TimestampClipStarted);
			ev["tags"] = Tags;
			Context->Events->Broadcast( "clip:new", std::move( ev ) );

			return true;
		});
	}

	auto WriteThumbnailMessage = std::make_shared<CameraWriteThumbnailMessage>( CameraID );
	
	std::string CachePath;

	{
		CachePath = Context->CachePath;
			
		auto CameraState = Context->FindCameraById( CameraID );
		if( CameraState )
		{
			WriteThumbnailMessage->Jpeg = CameraState->ClipThumbnails[ ClipStats.TimestampClipStarted ];

			if( WriteThumbnailMessage->Jpeg.empty() )
			{
				WriteThumbnailMessage->Jpeg = CameraState->PreviewThumbnail;
			}
		}
	}

	//Could get no image, in which case don't send it as we've got nothing to save out.
	if( !WriteThumbnailMessage->Jpeg.empty() )
	{	
		WriteThumbnailMessage->Filename = GetClipName( *Context, CameraID, ClipStats.TimestampClipStarted, false, false );
	
		Context->MessageBus->SendToClient( Worker.get(), WriteThumbnailMessage );
	}
}

void WitnessServer::StatusMessage( int Camera, std::string NewStatus, std::string Reason )
{
	std::string CameraName;

	{
		auto CameraState = Context->FindCameraById( Camera );
		if( CameraState )
		{
			if( NewStatus.length() > 0 )
			{
				CameraState->Status = NewStatus;
			}

			CameraName = CameraState->Name;
		}
	}

	LOG_INFO( "%s: %s", CameraName.c_str(), Reason.c_str() );
};