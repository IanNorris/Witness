#include "TimerWorker.h"
#include "Messages.h"

#include <fstream>
#include <windows.h>

void TimerWorker::WorkerMain()
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

	{
		lock_guard<mutex> Lock( Mutex );

		for( auto Iter = Triggers.begin(); Iter != Triggers.end(); ++Iter )
		{
			auto& Trigger = *Iter;
			if( TimeNow >= Trigger.LastTrigger + Trigger.Period )
			{
				Trigger.Callback();

				TimeNow = datetime::utc_timestamp();

				Trigger.LastTrigger = TimeNow;
			}
		}
	}

	Sleep(1000);
}
