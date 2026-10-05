#include "WorkerBase.h"

#ifdef _WIN32
#include <windows.h>
#endif

void WorkerBase::WorkerThread()
{
	MessageBusQueue = MessageBusObject->AddClient( this );

	WorkerInit();

	while( !Shutdown.load() )
	{
		UpdateLastTimedAction("Working...");

		WorkerMain();
	}

	MessageBusObject->RemoveClient( this );
	MessageBusQueue = nullptr;

	UpdateLastTimedAction("Shutting down...");

	WorkerShutdown();

	//Don't allow Complete to be fired until all work above is complete
	std::atomic_thread_fence(std::memory_order_seq_cst);

	Complete.store( true );

	UpdateLastTimedAction("Finished...");
}

void WorkerBase::SetPriority( Priority ThreadPriority )
{
#ifdef _WIN32
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
#else
	// Portable fallback: retain the process's normal scheduler policy. Do not
	// require elevated scheduling privileges just to start a worker on Linux.
	(void)ThreadPriority;
#endif
}
