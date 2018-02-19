#include "WatchdogWorker.h"
#include "Messages.h"

#include <windows.h>

void WatchdogWorker::WorkerMain()
{
	shared_ptr<Message> Msg;
	if( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
		{
			RequestShutdown();
		});
	}

	int64_t TimeNow = datetime::utc_timestamp();
	const int64_t WatchdogTime = 5;

	{
		lock_guard<mutex> Lock( Mutex );

		for( auto Iter = Targets.begin(); Iter != Targets.end(); ++Iter )
		{
			WorkerBase::AtomicTimedActionData Data = Iter->Thread->GetLastTimedAction()->load();

			if( TimeNow - WatchdogTime > (int64_t)Data.Timestamp )
			{
				tcerr << Iter->Name << ": Timeout of " << WatchdogTime << "s hit, last action was: " << Data.Action << endl;
			}
		}
	}

	Sleep(2000);
}
