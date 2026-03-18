#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"
#include "CrowListener.h"

void WitnessServer::HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data)
{
	std::shared_ptr<CameraWorker> Worker;
	std::string CameraName;

	{
		auto CameraState = Context->FindCameraById( Data.Camera );
		if(CameraState)
		{
			if( Data.Jpeg.size() )
			{
				CameraState->ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
			}
			else
			{
				LOG_WARNING( "Clip thumbnail is empty" );
			}
			CameraName = CameraState->Name;
			Worker = CameraState->Worker;

			CameraState->TriggeredActions.clear();

			//Already recording
			if (CameraState->IsRecording)
			{
				return;
			}

			CameraState->IsRecording = true;

			// Broadcast recording started
			crow::json::wvalue ev;
			ev["cameraID"] = Data.Camera;
			ev["recording"] = true;
			Context->Events->Broadcast( "camera:recording", std::move( ev ) );
		}
	}

	StartCameraRecording( Worker, Data.ClipStats.TimestampClipStarted, Data.Camera, false, Data.Result );
};

void WitnessServer::HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data)
{
	{	
		auto CameraState = Context->FindCameraById( Data.Camera );
		if( CameraState )
		{
			CameraState->ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
		}
	}
};

void WitnessServer::HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data)
{
	auto StopRecord = std::make_shared<CameraStopRecordMessage>( Data.Camera, false );

	std::shared_ptr<CameraWorker> Worker;

	{
		auto CameraState = Context->FindCameraById( Data.Camera );
		if( CameraState )
		{
			Worker = CameraState->Worker;

			if (CameraState->IsManualRecording)
			{
				return;
			}

			CameraState->IsRecording = false;

			// Broadcast recording stopped
			crow::json::wvalue ev;
			ev["cameraID"] = Data.Camera;
			ev["recording"] = false;
			Context->Events->Broadcast( "camera:recording", std::move( ev ) );
		}
	}

	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StopRecord );
	}
};
