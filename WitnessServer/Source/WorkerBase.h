#pragma once

#include <thread>

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
		const TCHAR* Action;
	};

	WorkerBase(const shared_ptr<MessageBus>& MessageBus)
	: MessageBusObject( MessageBus )
	, Shutdown( false )
	, Complete( false )
	{
	}

	void SetPriority( Priority ThreadPriority );

	void Start( Priority ThreadPriority )
	{
		UpdateLastTimedAction(_T("Thread starting..."));
		Thread = make_unique<thread>( &WorkerBase::WorkerThread, this );

		SetPriority(ThreadPriority);
	}

	void RequestShutdown()
	{
		Shutdown = true;
		if (MessageBusQueue)
		{
			MessageBusQueue->Push(make_shared<ThreadShutdownMessage>());
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

	const atomic<AtomicTimedActionData>* GetLastTimedAction() const { return &LastTimedAction; }

protected:

	shared_ptr<MessageBus> MessageBusObject;
	shared_ptr<MessageBusQueue> MessageBusQueue;

	void UpdateLastTimedAction( const TCHAR* NewAction )
	{
		LastTimedAction.store( AtomicTimedActionData{ datetime::utc_timestamp(), NewAction } );
	}

private:

	virtual void WorkerInit() {};
	virtual void WorkerShutdown() {};
	virtual void WorkerMain() = 0;

	void WorkerThread();

	atomic<AtomicTimedActionData> LastTimedAction;

	unique_ptr<thread> Thread;

	bool Shutdown;
	bool Complete;
};
