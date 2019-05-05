#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

#include <functional>
#include <windows.h>

#define MakeLambda( Name ) auto LambdaHandle##Name = bind( &WitnessServer::Handle##Name, this, placeholders::_1 )
#define HandleEvent( Name ) Msg->Handle<Name>(LambdaHandle##Name)

void WitnessServer::MessageLoop( bool& ContinueRunning )
{
	MakeLambda(CameraStartupMessage);
	MakeLambda(CameraReconnectMessage);
	MakeLambda(CameraConnectedMessage);
	MakeLambda(CameraSnapshotMessage);
	MakeLambda(CameraBeginMotionMessage);
	MakeLambda(CameraUpdateMotionMessage);
	MakeLambda(CameraEndMotionMessage);
		
	while( ContinueRunning )
	{
		shared_ptr<Message> Msg;
		MessageClient->Pop( Msg );

		HandleEvent(CameraStartupMessage);
		HandleEvent(CameraReconnectMessage);
		HandleEvent(CameraConnectedMessage);
		HandleEvent(CameraSnapshotMessage);
		HandleEvent(CameraBeginMotionMessage);
		HandleEvent(CameraUpdateMotionMessage);
		HandleEvent(CameraEndMotionMessage);

		Context->MessageBus->Forward<CameraWriteThumbnailMessage>( Worker.get(), Msg );
		
		Msg->Handle<CameraClipFinishedMessage>([&](const CameraClipFinishedMessage& Data)
		{
			StopCameraRecording( Data.ClipStats, Data.Camera, Data.Result );
		});

		Msg->Handle<CameraStateToggleRecordMessage>([&](const CameraStateToggleRecordMessage& Data)
		{
			StatusMessage( Data.Camera, _T(""), Data.Record ? _T("Manual Record: On") : _T("Manual Record: Off") );

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
				    uint64_t Timestamp = datetime::utc_timestamp();

					StartCameraRecording( Worker, Timestamp, Data.Camera, true, ClassificationResult() );
				}
				else
				{
					auto StopRecord = make_shared<CameraStopRecordMessage>( Data.Camera, true );
					Context->MessageBus->SendToClient( Worker.get(), StopRecord );
				}
			}
		});
	}
}
