#pragma once

#include "WorkerBase.h"

class WatchdogWorker : public WorkerBase
{
public:

	struct WatchdogTarget
	{
		std::shared_ptr<WorkerBase> Thread;
		StringT Name;
	};

	WatchdogWorker(const std::shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	{}

	void AddTarget(const std::shared_ptr<WorkerBase>& ThreadIn, const StringT& NameIn)
	{
		std::lock_guard<std::mutex> Lock( Mutex );

		Targets.push_back( WatchdogTarget{ ThreadIn, NameIn } );
	}

	void RemoveTarget(const std::shared_ptr<WorkerBase>& ThreadIn)
	{
		std::lock_guard<std::mutex> Lock( Mutex );

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

	mutable std::mutex Mutex;

	std::vector<WatchdogTarget> Targets;
};
