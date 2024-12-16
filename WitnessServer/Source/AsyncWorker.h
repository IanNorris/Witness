#pragma once

#include "WorkerBase.h"

class AsyncWorker : public WorkerBase
{
public:
	AsyncWorker(const std::shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	{}

private:

	void WorkerMain();
};
