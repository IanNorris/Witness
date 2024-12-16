#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"

void WitnessServer::HandleCameraStartupMessage(const CameraStartupMessage& Data)
{
	StatusMessage( Data.Camera, _T("Connecting"), _T("Connecting...") );
}

void WitnessServer::HandleCameraReconnectMessage(const CameraReconnectMessage& Data)
{
	StatusMessage( Data.Camera, _T("Reconnecting"), Data.Error );

	std::lock_guard<std::mutex> Lock( Context->Mutex );
			
	auto Iter = Context->Cameras.find( Data.Camera );
	if( Iter != Context->Cameras.end() )
	{
		(*Iter).second.IsRecording = false;
		(*Iter).second.IsManualRecording = false;
	}
}

void WitnessServer::HandleCameraConnectedMessage(const CameraConnectedMessage& Data)
{
	StatusMessage( Data.Camera, _T("Connected"), _T("Connected to camera") );
}

void WitnessServer::HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data)
{
	std::lock_guard<std::mutex> Lock( Context->Mutex );
			
	auto Iter = Context->Cameras.find( Data.Camera );
	if( Iter != Context->Cameras.end() )
	{
		(*Iter).second.PreviewThumbnail = Data.Jpeg;
	}
}
