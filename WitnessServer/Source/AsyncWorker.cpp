#include "AsyncWorker.h"
#include "Messages.h"

#include <fstream>
#include <windows.h>

void AsyncWorker::WorkerThread()
{
	MessageBusQueue = MessageBus->AddClient( this );

	auto& MB = *MessageBus;

	while( !Shutdown )
	{
		shared_ptr<Message> Msg;
		if( MessageBusQueue->TryPop( Msg ) )
		{
			Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
			{
				Shutdown = true;
			});

			Msg->Handle<CameraWriteThumbnailMessage>([&](const CameraWriteThumbnailMessage& Data)
			{
				ofstream Output( string( Data.Filename.begin(), Data.Filename.end() ), ofstream::binary );

				Output.write( (const char*)&Data.Jpeg[0], Data.Jpeg.size() );

				Output.close();
			});
		}
	}
	
	MessageBus->RemoveClient( this );
	MessageBusQueue = nullptr;

	//Don't allow Complete to be fired until all work above is complete
	MemoryBarrier();

	Complete = true;
}

void AsyncWorker::RequestShutdown()
{
	Shutdown = true;

	MemoryBarrier();
}
