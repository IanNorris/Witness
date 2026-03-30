#include "WatchdogWorker.h"
#include "Messages.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <atomic>
#endif

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
#ifdef _WIN32
	MemoryBarrier();
#else
	std::atomic_thread_fence( std::memory_order_seq_cst );
#endif

	Complete = true; 

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
	// Map to POSIX thread scheduling policy / nice value.
	int policy = SCHED_OTHER;
	struct sched_param param = {};

	switch (ThreadPriority)
	{
		case Priority::HighPriority:
			// Use SCHED_RR at a modest priority if permitted, fall back silently.
			param.sched_priority = 10;
			pthread_setschedparam( Thread->native_handle(), SCHED_RR, &param );
			break;

		case Priority::LowPriority:
			param.sched_priority = 0;
			pthread_setschedparam( Thread->native_handle(), SCHED_IDLE, &param );
			break;

		default:
			param.sched_priority = 0;
			pthread_setschedparam( Thread->native_handle(), SCHED_OTHER, &param );
			break;
	}
#endif
}