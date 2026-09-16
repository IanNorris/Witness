#pragma once

#include <thread>
#include <chrono>
#include <atomic>

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
		if( !Shutdown.exchange( true ) )
			MessageBusObject->SendToClient( this, std::make_shared<ThreadShutdownMessage>() );
	}

	void Join()
	{
		if( Thread && Thread->joinable() )
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

	bool IsShutdownRequested() const { return Shutdown.load(); }

private:

	virtual void WorkerInit() {};
	virtual void WorkerShutdown() {};
	virtual void WorkerMain() = 0;

	void WorkerThread();

	std::atomic<AtomicTimedActionData> LastTimedAction;

	std::unique_ptr<std::thread> Thread;

	std::atomic<bool> Shutdown;
	std::atomic<bool> Complete;
};
