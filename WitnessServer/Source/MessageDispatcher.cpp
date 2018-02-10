#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

#include <windows.h>

void WitnessServer::MessageLoop()
{
	string_t Errors;

	auto StartCameraRecording = [&](const shared_ptr<CameraWorker>& Worker, int CameraID, bool IsManual)
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
	};

	auto StopCameraRecording = [&](const ClipStatistics& ClipStats, int CameraID)
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
	};

	while( true )
	{
		shared_ptr<Message> Msg;
		MessageClient->Pop( Msg );

		auto StatusMessage = [&]( int Camera, string_t Reason )
		{
			string_t CameraName;

			{
				lock_guard<mutex> Lock( Context->Mutex );
			
				auto Iter = Context->Cameras.find( Camera );
				if( Iter != Context->Cameras.end() )
				{
					CameraName = (*Iter).second.Name;
				}
			}

			tcout << CameraName << _T(": ") << Reason << endl;
		};

		Msg->Handle<CameraStartupMessage>([&](const CameraStartupMessage& Data)
		{
			StatusMessage( Data.Camera, _T("Online") );
		});

		Msg->Handle<CameraShutdownMessage>([&](const CameraShutdownMessage& Data)
		{
			StatusMessage( Data.Camera, _T("Offline") );
		});

		Msg->Handle<CameraReconnectMessage>([&](const CameraReconnectMessage& Data)
		{
			StatusMessage( Data.Camera, Data.Error );

			{
				lock_guard<mutex> Lock( Context->Mutex );
			
				auto Iter = Context->Cameras.find( Data.Camera );
				if( Iter != Context->Cameras.end() )
				{
					(*Iter).second.IsRecording = false;
					(*Iter).second.IsManualRecording = false;
				}
			}
		});

		Msg->Handle<CameraSnapshotMessage>([&](const CameraSnapshotMessage& Data)
		{
			{
				lock_guard<mutex> Lock( Context->Mutex );
			
				auto Iter = Context->Cameras.find( Data.Camera );
				if( Iter != Context->Cameras.end() )
				{
					(*Iter).second.PreviewThumbnail = Data.Jpeg;
				}
			}
		});

		Msg->Handle<CameraBeginMotionMessage>([&](const CameraBeginMotionMessage& Data)
		{
			shared_ptr<CameraWorker> Worker;
			string_t CameraName;

			{
				lock_guard<mutex> Lock( Context->Mutex );
			
				auto Iter = Context->Cameras.find( Data.Camera );
				if( Iter != Context->Cameras.end() )
				{
					(*Iter).second.ClipThumbnails[ Data.Timestamp ] = Data.Jpeg;
					CameraName = (*Iter).second.Name;
					Worker = (*Iter).second.Worker;

					//Already recording
					if ((*Iter).second.IsRecording)
					{
						return;
					}

					(*Iter).second.IsRecording = true;
				}
			}

			stringstream_t Message;
			Message << _T("Begin Motion: ") << Data.MotionPercentage;

			StatusMessage( Data.Camera, Message.str() );

            stringstream_t ThumbPath;
            ThumbPath << Server->GetBaseUri() << _T("clip/thumb/") << Data.Camera << _T("/") << Data.Timestamp;
						
			SendAndroidNotification( Android.ServerKey, Android.TempUserId, _T("Camera info"), CameraName, ThumbPath.str(), nullptr );
						
			StartCameraRecording( Worker, Data.Camera, false );
		});

		Msg->Handle<CameraUpdateMotionMessage>([&](const CameraUpdateMotionMessage& Data)
		{
			{
				lock_guard<mutex> Lock( Context->Mutex );
			
				auto Iter = Context->Cameras.find( Data.Camera );
				if( Iter != Context->Cameras.end() )
				{
					(*Iter).second.ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
				}
			}
		});

		Msg->Handle<CameraEndMotionMessage>([&](const CameraEndMotionMessage& Data)
		{
			StatusMessage( Data.Camera, _T("End Motion") );

			auto StopRecord = make_shared<CameraStopRecordMessage>( Data.Camera );

			shared_ptr<CameraWorker> Worker;

			{
				lock_guard<mutex> Lock( Context->Mutex );

				auto Iter = Context->Cameras.find( Data.Camera );
				if( Iter != Context->Cameras.end() )
				{
					Worker = (*Iter).second.Worker;

					if ((*Iter).second.IsManualRecording)
					{
						return;
					}

					(*Iter).second.IsRecording = false;
				}
			}

			if( Worker )
			{
				Context->MessageBus->SendToClient( Worker.get(), StopRecord );
			}
		});

		
		Msg->Handle<CameraClipFinishedMessage>([&](const CameraClipFinishedMessage& Data)
		{
			StopCameraRecording( Data.ClipStats, Data.Camera );
		});

		Msg->Handle<CameraStateToggleRecordMessage>([&](const CameraStateToggleRecordMessage& Data)
		{
			StatusMessage( Data.Camera, Data.Record ? _T("Manual Record: On") : _T("Manual Record: Off") );

			shared_ptr<CameraWorker> Worker;

			bool Change = false;

			{
				lock_guard<mutex> Lock( Context->Mutex );

				auto Iter = Context->Cameras.find( Data.Camera );
				if( Iter != Context->Cameras.end() )
				{
					Worker = (*Iter).second.Worker;
					(*Iter).second.IsManualRecording = Data.Record;

					if( (*Iter).second.IsRecording != Data.Record )
					{
						(*Iter).second.IsRecording = Data.Record;
						Change = true;
					}
				}
			}

			if( Change && Worker )
			{
				if( Data.Record )
				{
					StartCameraRecording( Worker, Data.Camera, true );
				}
				else
				{
					auto StopRecord = make_shared<CameraStopRecordMessage>( Data.Camera );
					Context->MessageBus->SendToClient( Worker.get(), StopRecord );
				}
			}
		});
	}
}
