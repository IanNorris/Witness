#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

#include <functional>
#include <windows.h>

#define MakeLambda( Name ) auto LambdaHandle##Name = std::bind( &WitnessServer::Handle##Name, this, std::placeholders::_1 )
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
		std::shared_ptr<Message> Msg;
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

			std::shared_ptr<CameraWorker> Worker;

			bool Change = false;

			{
				auto CameraState = Context->FindCameraById( Data.Camera );
				if(CameraState)
				{
					Worker = CameraState->Worker;
					CameraState->IsManualRecording = Data.Record;

					if( CameraState->IsRecording != Data.Record )
					{
						CameraState->IsRecording = Data.Record;
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
					auto StopRecord = std::make_shared<CameraStopRecordMessage>( Data.Camera, true );
					Context->MessageBus->SendToClient( Worker.get(), StopRecord );
				}
			}
		});

		Msg->Handle< CameraAddedMessage>([&](const CameraAddedMessage& Data)
		{
			std::lock_guard<std::mutex> Lock(Context->Mutex);

			MAKE_QUERY(GetCamera);
			GetCamera->Bind("@CameraId", Data.Camera);

			GetCamera->Execute(
				[&](const SQLiteDatabaseQuery& query)
				{
					StartCamera(query);

					Context->LongPoll->NotifyAll();

					return true;
				}
			);
		});

		Msg->Handle<CameraRemovedMessage>([&](const CameraRemovedMessage& Data)
		{
			std::shared_ptr<CameraWorker> Worker;

			{
				auto CameraState = Context->FindCameraById(Data.Camera);
				if (CameraState)
				{
					Worker = CameraState->Worker;
				}
			}

			if (Worker)
			{
				Watchdog->RemoveTarget(Worker);

				Context->MessageBus->SendToClient(Worker.get(), std::make_shared<ThreadShutdownMessage>());

				Context->LongPoll->NotifyAll();
			}
		});
	}
}
