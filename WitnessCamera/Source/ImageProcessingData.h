#pragma once

#include <mutex>
#include <shared_mutex>
#include <vector>
#include <memory>
#include <unordered_map>

#include "RecordFilter.h"
#include "SourceStats.h"

struct SwsContext;

namespace Witness{
namespace Camera{

class IRecordFilter;

struct SourceState
{
	SourceState();
	~SourceState();

	std::shared_ptr<FilterFrameContext> FrameContext;

	bool HasViewerFullSize;
	bool HasViewerPreviewSize;
};

struct ImageProcessingJobQueueData
{
	std::mutex											QueueMutex;
	std::condition_variable								Condition;
	std::vector<SharedClassificationTask>				Queue;
	std::vector<SharedClassificationTask>				HighPriorityAsyncQueue;
	std::vector<int>									ActiveSources;

	std::mutex											StateMutex;
	std::unordered_map<int,std::shared_ptr<SourceState>> States;

	std::mutex											StatsMutex;
	std::unordered_map<int, SourceStats>				Stats;

	void AddFrame(int Source, int64_t Timestamp, const FilterFrameStats& StatsIn );
	void ResetStats(int Source);

	std::shared_ptr<SourceState> GetStateForSource(int Source)
	{
		std::lock_guard<std::mutex> Lock(StateMutex);

		auto State = States[Source];
		if (State == nullptr)
		{
			State = States[Source] = std::make_shared<SourceState>();
		}

		return State;
	}

	SourceStats GetStatsForSource(int Source)
	{
		std::lock_guard<std::mutex> Lock(StateMutex);

		return Stats[Source];
	}
};

}}