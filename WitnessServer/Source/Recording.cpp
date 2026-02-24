#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "Commands/Clip.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

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
	string_t Tags( TagsA.begin(), TagsA.end() );

	std::filesystem::create_directories( std::filesystem::path(CachePath) );
	stringstream_t TargetFilename;
	TargetFilename << CameraID << (IsManual ? _T("_Manual_") : _T("_Auto_")) << Timestamp << ".mp4";
	auto TargetPath = (std::filesystem::path(CachePath) / TargetFilename.str()).native();

	auto StartRecord = std::make_shared<CameraStartRecordMessage>( CameraID, Timestamp, TargetPath );
			
	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StartRecord );
	}

	SQLiteDatabaseQueryInstance CreateClip( Context->Database, _T("CreateClip") );
	CreateClip->Bind( "@Timestamp", (int64_t)Timestamp );
	CreateClip->Bind( "@Camera", CameraID );
	CreateClip->Bind( "@ActiveDuration", 0 );
	CreateClip->Bind( "@Duration", 0 );
	CreateClip->Bind( "@RecordMode", IsManual ? 0 : 1 );
	CreateClip->Bind( "@MaxMotion", 0.0f );
	CreateClip->Bind( "@Description", _T("") );
	CreateClip->Bind( "@Save", 0 );
	CreateClip->Bind( "@Tags", Tags.c_str() );
	CreateClip->Execute( nullptr );
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
	string_t Tags( TagsA.begin(), TagsA.end() );

	SQLiteDatabaseQueryInstance UpdateClip( Context->Database, _T("UpdateClip") );
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

	auto WriteThumbnailMessage = std::make_shared<CameraWriteThumbnailMessage>( CameraID );
	
	string_t CachePath;

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

void WitnessServer::StatusMessage( int Camera, string_t NewStatus, string_t Reason )
{
	string_t CameraName;

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

	std::tcout << CameraName << _T(": ") << Reason << std::endl;
};