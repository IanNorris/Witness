#include "AsyncWorker.h"
#include "Messages.h"

#include <fstream>

void AsyncWorker::WorkerMain()
{
	std::shared_ptr<Message> Msg;
	MessageBusQueue->Pop( Msg );

	Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
	{
		RequestShutdown();
	});

	Msg->Handle<CameraWriteThumbnailMessage>([&](const CameraWriteThumbnailMessage& Data)
	{
		std::ofstream Output(Data.Filename, std::ofstream::binary );

		Output.write( (const char*)&Data.Jpeg[0], Data.Jpeg.size() );

		Output.close();
	});
}
