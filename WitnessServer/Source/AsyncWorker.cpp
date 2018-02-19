#include "AsyncWorker.h"
#include "Messages.h"

#include <fstream>
#include <windows.h>

void AsyncWorker::WorkerMain()
{
	shared_ptr<Message> Msg;
	MessageBusQueue->Pop( Msg );

	Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
	{
		RequestShutdown();
	});

	Msg->Handle<CameraWriteThumbnailMessage>([&](const CameraWriteThumbnailMessage& Data)
	{
		ofstream Output( string( Data.Filename.begin(), Data.Filename.end() ), ofstream::binary );

		Output.write( (const char*)&Data.Jpeg[0], Data.Jpeg.size() );

		Output.close();
	});
}
