#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"

void WitnessServer::HandleCameraStartupMessage(const CameraStartupMessage& Data)
{
	StatusMessage( Data.Camera, _T("Online") );
};

void WitnessServer::HandleCameraReconnectMessage(const CameraReconnectMessage& Data)
{
	StatusMessage( Data.Camera, Data.Error );

	lock_guard<mutex> Lock( Context->Mutex );
			
	auto Iter = Context->Cameras.find( Data.Camera );
	if( Iter != Context->Cameras.end() )
	{
		(*Iter).second.IsRecording = false;
		(*Iter).second.IsManualRecording = false;
	}
};

void WitnessServer::HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data)
{
	lock_guard<mutex> Lock( Context->Mutex );
			
	auto Iter = Context->Cameras.find( Data.Camera );
	if( Iter != Context->Cameras.end() )
	{
		(*Iter).second.PreviewThumbnail = Data.Jpeg;
	}
};
