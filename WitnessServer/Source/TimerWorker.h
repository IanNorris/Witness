#pragma once

#include "WorkerBase.h"
#include "GlobalContext.h"

class TimerWorker : public WorkerBase
{
public:

	struct TimedTrigger
	{
		function<void()> Callback;
		int Period;
		int64_t LastTrigger;
	};

	TimerWorker(const shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	{}

	void AddTimer(function<void()> Callback,int Period)
	{
		lock_guard<mutex> Lock( Mutex );

		TimedTrigger Trigger;
		Trigger.Callback = Callback;
		Trigger.Period = Period;
		Trigger.LastTrigger = 0;

		Triggers.push_back( Trigger );
	}

private:

	virtual void WorkerMain() override;

	mutable mutex Mutex;

	vector<TimedTrigger> Triggers;
};
