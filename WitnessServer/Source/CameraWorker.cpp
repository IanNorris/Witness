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

			Message->Handle<CameraStartRecordMessage>([&](const CameraStartRecordMessage& Data)
			{
				if (RecordStream)
				{
					RecordStream->CloseFile();
					RecordStream.reset();
				}
				
				RecordStream = make_shared<OutputStream>( string( Data.Path.begin(), Data.Path.end() ), CameraStream.get() );
				RecordStream->Initialize();
			});

			Message->Handle<CameraStopRecordMessage>([&](const CameraStopRecordMessage& Data)
			{
				if( RecordStream )
				{
					RecordStream->CloseFile();
					RecordStream.reset();
				}
			});
		}

		CameraStreamError Error = CameraStream->ProcessFrame( Filter.get(), RecordStream.get() );
		if( Error != CameraStreamError::Success )
		{
			if( Error == CameraStreamError::EndOfFile && RecordStream )
			{
				RecordStream->CloseFile();
				RecordStream.reset();
			}

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
