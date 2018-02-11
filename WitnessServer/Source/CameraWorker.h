#pragma once

#include <thread>

#include "Common.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

#include "MessageBus.h"

class ObservingMotionFilter;

using namespace Witness::Camera;

class CameraWorker
{
public:
	CameraWorker(const int CameraID, const string_t& InputPath, const shared_ptr<MessageBus>& MessageBus)
	: MessageBus( MessageBus )
	, Path( InputPath )
	, CameraID( CameraID )
	, Shutdown( false )
	, Complete( false )
	{
		Thread = make_unique<thread>( &CameraWorker::WorkerThread, this );
	}

	void RequestShutdown();

	virtual ~CameraWorker()
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

	shared_ptr<OutputStream> RecordStream;

	shared_ptr<InputStream> CameraStream;
	shared_ptr<ObservingMotionFilter> Filter;

	shared_ptr<MessageBus> MessageBus;
	shared_ptr<MessageBusQueue> MessageBusQueue;

	string_t Path;
	int CameraID;
	bool Shutdown;
	bool Complete;
};
