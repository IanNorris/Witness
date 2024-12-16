#include "ImageProcessorWorker.h"
#include "Messages.h"

void ImageProcessorWorker::WorkerMain()
{
	std::shared_ptr<Message> Msg;
	if( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
		{
			RequestShutdown();
		});
	}

	JobQueue->WorkerThreadMain();
}
