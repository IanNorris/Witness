#include "WatchdogWorker.h"
#include "Messages.h"

#include <Log.h>
#include <chrono>
#include <thread>

void WatchdogWorker::WorkerMain()
{
	std::shared_ptr<Message> Msg;
	if( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
		{
			RequestShutdown();
		});
	}

	int64_t TimeNow = GetUnixTimestamp();
	const int64_t WatchdogTime = 5;

	{
		std::lock_guard<std::mutex> Lock( Mutex );

		for( auto Iter = Targets.begin(); Iter != Targets.end(); ++Iter )
		{
			WorkerBase::AtomicTimedActionData Data = Iter->Thread->GetLastTimedAction()->load();

			if( TimeNow - WatchdogTime > (int64_t)Data.Timestamp )
			{
				LOG_WARNING( "%s: Timeout of %llds hit, last action was: %s", Iter->Name.c_str(), (long long)WatchdogTime, Data.Action );
			}
		}
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(2000));
}
