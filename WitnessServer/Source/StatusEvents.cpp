#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"

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
	}
}

void WitnessServer::HandleCameraConnectedMessage(const CameraConnectedMessage& Data)
{
	StatusMessage( Data.Camera, "Connected", "Connected to camera" );
}

void WitnessServer::HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data)
{
	auto CameraState = Context->FindCameraById( Data.Camera );
	if(CameraState)
	{
		CameraState->PreviewThumbnail = Data.Jpeg;
	}
}
