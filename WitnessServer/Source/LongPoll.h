#pragma once

#include <mutex>
#include <memory>
#include <condition_variable>

class LongPollDispatch
{
public:

	LongPollDispatch()
	: ListenerCount( 0 )
	{}

	std::unique_lock<std::mutex> EnterLock()
	{
		std::unique_lock<std::mutex> Lock(Mutex);

		ListenerCount++;

		return std::move(Lock);
	}

	void ExitLock(std::unique_lock<std::mutex>& Lock)
	{
		ListenerCount--;
	}

	void NotifyAll()
	{
		bool ShouldNotify = false;

		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (ListenerCount)
			{
				ShouldNotify = true;
			}
		}

		if (ShouldNotify)
		{
			Condition.notify_all();
		}
	}

	void Wait(std::unique_lock<std::mutex>& Lock)
	{
		Condition.wait(Lock);
	}

private:

	mutable std::mutex Mutex;

	std::condition_variable Condition;
	int ListenerCount;
};

class LongPollScope
{
public:

	LongPollScope( shared_ptr<LongPollDispatch> InDispatcher)
	: Dispatcher( InDispatcher )
	, Lock( Dispatcher->EnterLock() )
	{
	}

	~LongPollScope()
	{
		Dispatcher->ExitLock( Lock );
	}

	void Wait()
	{
		Dispatcher->Wait(Lock);
	}

private:

	const std::shared_ptr<LongPollDispatch> Dispatcher;
	std::unique_lock<std::mutex> Lock;
};