#include "CameraWorker.h"
#include "ObservingMotionFilter.h"

#include <windows.h>

void CameraWorker::WorkerThread()
{
	MessageBusQueue = MessageBus->AddClient( this );
	Filter = make_shared<ObservingMotionFilter>( CameraID, MessageBus );
	CameraStream = make_shared<InputStream>( std::string( Path.begin(), Path.end() ) );

	MessageBus->SendToClient( nullptr, make_shared<CameraStartupMessage>( CameraID ) );

	while( !Shutdown )
	{
		shared_ptr<Message> Message;
		if( MessageBusQueue->TryPop( Message ) )
		{
			Message->Handle<CameraShutdownMessage>([&](const CameraShutdownMessage& Data)
			{
				Shutdown = true;
			});
		}

		CameraStreamError Error = CameraStream->ProcessFrame( Filter.get(), nullptr );
		if( Error != CameraStreamError::Success )
		{
			MessageBus->SendToClient( nullptr, make_shared<CameraReconnectMessage>( CameraID, GetCameraStreamErrorMessage(Error) ) );

			CameraStream = make_shared<InputStream>( std::string( Path.begin(), Path.end() ) );
		}
	}

	//Ensure destruction is done on the worker thread
	Filter = nullptr;
	CameraStream = nullptr;

	MessageBus->SendToClient( nullptr, make_shared<CameraShutdownMessage>( CameraID ) );

	MessageBus->RemoveClient( this );
	MessageBusQueue = nullptr;

	//Don't allow Complete to be fired until all work above is complete
	MemoryBarrier();

	Complete = true;
}

void CameraWorker::RequestShutdown()
{
	Shutdown = true;

	MemoryBarrier();
}
