#pragma once

#include <mutex>
#include <shared_mutex>
#include <vector>
#include <memory>
#include <unordered_map>

namespace Witness{
namespace Camera{

class IRecordFilter;

struct ImageProcessingJob
{
	std::shared_ptr<FFMPEG::Frame>	Frame;
	std::shared_ptr<IRecordFilter>	Filter;

	int64_t							Timestamp;

	unsigned int					TargetHeight;
	int								SourceID;
};

struct SourceStats
{
	SourceStats()
	{
		Reset();
	}

	int64_t							LastTimestamp;
	int64_t							FrameCount;
	int64_t							TotalProcessingTime;
	int64_t							ScaleTotalProcessingTime;
	int64_t							MotionDetectionTotalProcessingTime;
	int64_t							SecondPassFilterTotalProcessingTime;
	
	void Reset()
	{
		LastTimestamp = 0;
		FrameCount = 0;
		TotalProcessingTime = 0;
		ScaleTotalProcessingTime = 0;
		MotionDetectionTotalProcessingTime = 0;
		SecondPassFilterTotalProcessingTime = 0;
	}
};

struct SourceState
{
	SourceState();
	~SourceState();

	SwsContext* ConversionContext;
};

struct ImageProcessingJobQueueData
{
	std::mutex											QueueMutex;
	std::condition_variable								Condition;
	std::vector<std::shared_ptr<ImageProcessingJob>>	Queue;
	std::vector<int>									ActiveSources;

	std::mutex											StateMutex;
	std::unordered_map<int,std::shared_ptr<SourceState>> States;

	std::mutex											StatsMutex;
	std::unordered_map<int, SourceStats>				Stats;

	void AddFrame(int Source, int64_t Timestamp, int64_t ScaleProcessingTime, int64_t MDProcessingTime, int64_t SecondPassProcessingTime);

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