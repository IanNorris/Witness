#include "TimerWorker.h"

#include <chrono>

void TimerWorker::WorkerMain()
{
	std::shared_ptr<Message> Msg;
	if( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage&)
		{
			RequestShutdown();
		});
	}

	int64_t TimeNow = GetUnixTimestamp();

	{
		std::lock_guard<std::mutex> Lock( Mutex );

		for( auto Iter = Triggers.begin(); Iter != Triggers.end(); ++Iter )
		{
			auto& Trigger = *Iter;
			if( TimeNow >= Trigger.LastTrigger + Trigger.Period )
			{
				Trigger.Callback();

				TimeNow = GetUnixTimestamp();

				Trigger.LastTrigger = TimeNow;
			}
		}
	}

	WaitForShutdown(std::chrono::milliseconds(1000));
}
