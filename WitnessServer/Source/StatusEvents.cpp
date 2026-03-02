#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"

#include <Log.h>

void WitnessServer::HandleCameraStartupMessage(const CameraStartupMessage& Data)
{
	StatusMessage( Data.Camera, "Connecting", "Connecting..." );
}

void WitnessServer::HandleCameraReconnectMessage(const CameraReconnectMessage& Data)
{
	StatusMessage( Data.Camera, "Reconnecting", Data.Error );

	auto CameraState = Context->FindCameraById( Data.Camera );
	if(CameraState)
	{
		CameraState->IsRecording = false;
		CameraState->IsManualRecording = false;

		crow::json::wvalue ev;
		ev["cameraID"] = Data.Camera;
		ev["status"] = "Reconnecting";
		ev["recording"] = false;
		Context->Events->Broadcast( "camera:state", std::move( ev ) );
	}
}

void WitnessServer::HandleCameraConnectedMessage(const CameraConnectedMessage& Data)
{
	StatusMessage( Data.Camera, "Connected", "Connected to camera" );

	{
		crow::json::wvalue ev;
		ev["cameraID"] = Data.Camera;
		ev["status"] = "Connected";
		Context->Events->Broadcast( "camera:state", std::move( ev ) );
	}

	if( !AllCamerasReported )
	{
		std::lock_guard<std::mutex> lock( Context->Mutex );
		auto& cameras = Context->GetCameraMap();
		int total = (int)cameras.size();
		int connected = 0;
		for( auto& [id, state] : cameras )
		{
			if( state.Status == "Connected" )
				connected++;
		}
		if( connected == total && total > 0 )
		{
			AllCamerasReported = true;
			LOG_INFO( "All %d cameras online - server ready", total );
		}
	}
}

void WitnessServer::HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data)
{
	auto CameraState = Context->FindCameraById( Data.Camera );
	if(CameraState)
	{
		CameraState->PreviewThumbnail = Data.Jpeg;
	}
}
