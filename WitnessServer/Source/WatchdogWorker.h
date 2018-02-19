#pragma once

#include "WorkerBase.h"

class WatchdogWorker : public WorkerBase
{
public:

	struct WatchdogTarget
	{
		shared_ptr<WorkerBase> Thread;
		string_t Name;
	};

	WatchdogWorker(const shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	{}

	void AddTarget(const shared_ptr<WorkerBase>& ThreadIn, const string_t& NameIn)
	{
		lock_guard<mutex> Lock( Mutex );

		Targets.push_back( WatchdogTarget{ ThreadIn, NameIn } );
	}

	void RemoveTarget(const shared_ptr<WorkerBase>& ThreadIn)
	{
		lock_guard<mutex> Lock( Mutex );

		for( auto Iter = Targets.begin(); Iter != Targets.end(); ++Iter )
		{
			if ( ThreadIn == ((*Iter).Thread ) )
			{
				Targets.erase( Iter );
				break;
			}
		}
	}

private:

	virtual void WorkerMain() override;

	mutable mutex Mutex;

	vector<WatchdogTarget> Targets;
};
