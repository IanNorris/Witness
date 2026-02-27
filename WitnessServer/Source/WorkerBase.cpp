#include "WatchdogWorker.h"
#include "Messages.h"

#include <windows.h>

void WorkerBase::WorkerThread()
{
	MessageBusQueue = MessageBusObject->AddClient( this );

	WorkerInit();

	while( !Shutdown )
	{
		UpdateLastTimedAction("Working...");

		WorkerMain();
	}

	MessageBusObject->RemoveClient( this );
	MessageBusQueue = nullptr;

	UpdateLastTimedAction("Shutting down...");

	WorkerShutdown();

	//Don't allow Complete to be fired until all work above is complete
	MemoryBarrier();

	Complete = true; 

	UpdateLastTimedAction("Finished...");
}

void WorkerBase::SetPriority( Priority ThreadPriority )
{
	int PlatformPriority = THREAD_PRIORITY_NORMAL;
	switch (ThreadPriority)
	{
		case Priority::HighPriority:
			PlatformPriority = THREAD_PRIORITY_ABOVE_NORMAL;
			break;

		case Priority::LowPriority:
			PlatformPriority = THREAD_PRIORITY_BELOW_NORMAL;
			break;

		default:
			PlatformPriority = THREAD_PRIORITY_NORMAL;
			break;
	}
		
	SetThreadPriority( Thread->native_handle(), PlatformPriority );
}