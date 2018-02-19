#pragma once

#include "WorkerBase.h"

class AsyncWorker : public WorkerBase
{
public:
	AsyncWorker(const shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	{}

private:

	void WorkerMain();
};
