#include "WatchdogWorker.h"
#include "Messages.h"

#include <windows.h>

void WorkerBase::WorkerThread()
{
	MessageBusQueue = MessageBusObject->AddClient( this );

	WorkerInit();

	while( !Shutdown )
	{
		UpdateLastTimedAction(_T("Working..."));

		WorkerMain();
	}

	MessageBusObject->RemoveClient( this );
	MessageBusQueue = nullptr;

	UpdateLastTimedAction(_T("Shutting down..."));

	WorkerShutdown();

	//Don't allow Complete to be fired until all work above is complete
	MemoryBarrier();

	Complete = true; 

	UpdateLastTimedAction(_T("Finished..."));
}
