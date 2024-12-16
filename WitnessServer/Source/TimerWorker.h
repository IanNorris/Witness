#pragma once

#include "WorkerBase.h"
#include "GlobalContext.h"

class TimerWorker : public WorkerBase
{
public:

	struct TimedTrigger
	{
		std::function<void()> Callback;
		int Period;
		int64_t LastTrigger;
	};

	TimerWorker(const std::shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	{}

	void AddTimer(std::function<void()> Callback,int Period)
	{
		std::lock_guard<std::mutex> Lock( Mutex );

		TimedTrigger Trigger;
		Trigger.Callback = Callback;
		Trigger.Period = Period;
		Trigger.LastTrigger = 0;

		Triggers.push_back( Trigger );
	}

private:

	virtual void WorkerMain() override;

	mutable std::mutex Mutex;

	std::vector<TimedTrigger> Triggers;
};
