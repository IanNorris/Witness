#pragma once

#include <thread>
#include <chrono>

#include "Common.h"

#include "MessageBus.h"

class WorkerBase
{
public:

	enum class Priority
	{
		HighPriority,
		Normal,
		LowPriority
	};

	struct AtomicTimedActionData
	{
		uint64_t Timestamp;
		const char* Action;
	};

	WorkerBase(const std::shared_ptr<MessageBus>& MessageBus)
	: MessageBusObject( MessageBus )
	, Shutdown( false )
	, Complete( false )
	{
	}

	void SetPriority( Priority ThreadPriority );

	void Start( Priority ThreadPriority )
	{
		UpdateLastTimedAction("Thread starting...");
		Thread = std::make_unique<std::thread>( &WorkerBase::WorkerThread, this );

		SetPriority(ThreadPriority);
	}

	void RequestShutdown()
	{
		Shutdown = true;
		if (MessageBusQueue)
		{
			MessageBusQueue->Push(std::make_shared<ThreadShutdownMessage>());
		}
	}

	void Join()
	{
		Thread->join();
	}


	virtual ~WorkerBase()
	{
		RequestShutdown();

		Join();
	}

	const std::atomic<AtomicTimedActionData>* GetLastTimedAction() const { return &LastTimedAction; }

protected:

	std::shared_ptr<MessageBus> MessageBusObject;
	std::shared_ptr<MessageBusQueue> MessageBusQueue;

	void UpdateLastTimedAction( const char* NewAction )
	{
		LastTimedAction.store( AtomicTimedActionData{ static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch() ).count()), NewAction } );
	}

	bool IsShutdownRequested() const { return Shutdown; }

private:

	virtual void WorkerInit() {};
	virtual void WorkerShutdown() {};
	virtual void WorkerMain() = 0;

	void WorkerThread();

	std::atomic<AtomicTimedActionData> LastTimedAction;

	std::unique_ptr<std::thread> Thread;

	bool Shutdown;
	bool Complete;
};
