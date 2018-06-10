#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "Commands/Clip.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

#include <windows.h>

void WitnessServer::StartCameraRecording( const shared_ptr<CameraWorker>& Worker, int CameraID, bool IsManual )
{
	int64_t Timestamp = datetime::utc_timestamp();

	CreateDirectoryW( CachePath.c_str(), nullptr );
	stringstream_t TargetFilename;
	TargetFilename << CachePath << _T("\\") << CameraID << (IsManual ? _T("_Manual_") : _T("_Auto_")) << Timestamp << ".mp4";

	auto StartRecord = make_shared<CameraStartRecordMessage>( CameraID, Timestamp, TargetFilename.str() );
			
	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StartRecord );
	}

	SQLiteDatabaseQueryInstance CreateClip( Context->Database, _T("CreateClip") );
	CreateClip->Bind( "@Timestamp", Timestamp );
	CreateClip->Bind( "@Camera", CameraID );
	CreateClip->Bind( "@ActiveDuration", 0 );
	CreateClip->Bind( "@Duration", 0 );
	CreateClip->Bind( "@RecordMode", IsManual ? 0 : 1 );
	CreateClip->Bind( "@MaxMotion", 0.0f );
	CreateClip->Bind( "@Description", _T("") );
	CreateClip->Execute( nullptr );
}

void WitnessServer::StopCameraRecording( const ClipStatistics& ClipStats, int CameraID )
{
	SQLiteDatabaseQueryInstance UpdateClip( Context->Database, _T("UpdateClip") );
	UpdateClip->Bind( "@Timestamp", (int64_t)ClipStats.TimestampClipStarted );
	UpdateClip->Bind( "@Camera", CameraID );

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

	auto WriteThumbnailMessage = make_shared<CameraWriteThumbnailMessage>( CameraID );
	
	string_t CachePath;

	{
		lock_guard<mutex> Lock( Context->Mutex );

		CachePath = Context->CachePath;
			
		auto Iter = Context->Cameras.find( CameraID );
		if( Iter != Context->Cameras.end() )
		{
			WriteThumbnailMessage->Jpeg = (*Iter).second.ClipThumbnails[ ClipStats.TimestampClipStarted ];

			if( WriteThumbnailMessage->Jpeg.empty() )
			{
				WriteThumbnailMessage->Jpeg = (*Iter).second.PreviewThumbnail;
			}
		}
	}

	//Could get no image, in which case don't send it as we've got nothing to save out.
	if( !WriteThumbnailMessage->Jpeg.empty() )
	{	
		WriteThumbnailMessage->Filename = GetClipName( *Context, CameraID, ClipStats.TimestampClipStarted, false, false );;
	
		Context->MessageBus->SendToClient( Worker.get(), WriteThumbnailMessage );
	}
}

void WitnessServer::StatusMessage( int Camera, string_t NewStatus, string_t Reason )
{
	string_t CameraName;

	{
		lock_guard<mutex> Lock( Context->Mutex );
			
		auto Iter = Context->Cameras.find( Camera );
		if( Iter != Context->Cameras.end() )
		{
			if( NewStatus.length() > 0 )
			{
				(*Iter).second.Status = NewStatus;
			}

			CameraName = (*Iter).second.Name;
		}
	}

	tcout << CameraName << _T(": ") << Reason << endl;
};