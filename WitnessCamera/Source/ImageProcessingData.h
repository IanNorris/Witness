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
	// Stable source order provides fair round-robin dispatch. Each source has
	// at most one replaceable ingress frame in Queue; continuations are retained.
	std::vector<int>									EssentialSourceOrder;
	size_t										NextEssentialSource = 0;
	// Candidate frames are replaceable. Once inference has started, its
	// continuations go here and are never replaced by a newer candidate.
	std::vector<SharedClassificationTask>				AIQueue;
	std::vector<SharedClassificationTask>				AIContinuationQueue;
	std::vector<int>									ActiveSources;
	std::vector<int>									ActiveAISources;
	size_t										MaximumConcurrentAIJobs = 1;
	size_t										ActiveBackgroundAIJobs = 0;
	std::unordered_map<int, uint64_t>					SourceGenerations;

	std::mutex											StateMutex;
	std::unordered_map<int,std::shared_ptr<SourceState>> States;

	std::mutex											StatsMutex;
	std::unordered_map<int, SourceStats>				Stats;

	bool												WantExit;

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
		std::lock_guard<std::mutex> Lock(StatsMutex);

		return Stats[Source];
	}
};

}}
