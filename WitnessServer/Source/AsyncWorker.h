#pragma once

#include <thread>

#include "Common.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

#include "MessageBus.h"

class ObservingMotionFilter;

using namespace Witness::Camera;

class AsyncWorker
{
public:
	AsyncWorker(const shared_ptr<MessageBus>& MessageBus)
	: MessageBus( MessageBus )
	, Shutdown( false )
	, Complete( false )
	{
		Thread = make_unique<thread>( &AsyncWorker::WorkerThread, this );
	}

	void RequestShutdown();

	virtual ~AsyncWorker()
	{
		RequestShutdown();

		if( !Complete )
		{
			Thread->join();
		}
	}

private:

	void WorkerThread();

	unique_ptr<thread> Thread;

	shared_ptr<MessageBus> MessageBus;
	shared_ptr<MessageBusQueue> MessageBusQueue;

	bool Shutdown;
	bool Complete;
};
