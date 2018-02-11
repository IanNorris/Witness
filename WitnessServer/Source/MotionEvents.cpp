#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"
#include "Listener.h"
#include "Android/AndroidNotify.h"

void WitnessServer::HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data)
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
};

void WitnessServer::HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data)
{
	{
		lock_guard<mutex> Lock( Context->Mutex );
			
		auto Iter = Context->Cameras.find( Data.Camera );
		if( Iter != Context->Cameras.end() )
		{
			(*Iter).second.ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
		}
	}
};

void WitnessServer::HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data)
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
};
