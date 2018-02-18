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

void WitnessServer::MessageLoop()
{
	MakeLambda(CameraStartupMessage);
	MakeLambda(CameraReconnectMessage);
	MakeLambda(CameraSnapshotMessage);
	MakeLambda(CameraBeginMotionMessage);
	MakeLambda(CameraUpdateMotionMessage);
	MakeLambda(CameraEndMotionMessage);
		
	while( true )
	{
		shared_ptr<Message> Msg;
		MessageClient->Pop( Msg );

		HandleEvent(CameraStartupMessage);
		HandleEvent(CameraReconnectMessage);
		HandleEvent(CameraSnapshotMessage);
		HandleEvent(CameraBeginMotionMessage);
		HandleEvent(CameraUpdateMotionMessage);
		HandleEvent(CameraEndMotionMessage);

		Context->MessageBus->Forward<CameraWriteThumbnailMessage>( Worker.get(), Msg );
		
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
