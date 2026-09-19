#pragma once

#include "Export.h"
#include <stdint.h>
#include <memory.h>
#include <chrono>

enum FilterStat
{
	FilterStat_Process_Total,

	FilterStat_JpegEncoding,

	FilterStat_Scale,
	FilterStat_ObserverFilter,
	FilterStat_FirstPassFilter,
	FilterStat_SecondPassFilter,
	FilterStat_ThirdPassFilter,

	FilterStat_MVF_Internal,
	FilterStat_MVF_SideData,
	FilterStat_MVF_VectorPass,
	FilterStat_MVF_ClusterPass,
	FilterStat_MVF_ObjectPass,

	FilterStat_Debug,

	FilterStat_Max
};

struct FilterFrameStats
{
	FilterFrameStats()
	{
		Reset();
	}

	void Reset()
	{
		memset( Stats, 0, sizeof(Stats) );
		memset( FrameCount, 0, sizeof(FrameCount) );
		memset( WasHit, 0, sizeof(WasHit) );
	}

	int64_t Stats[FilterStat_Max];
	int64_t FrameCount[FilterStat_Max];
	bool WasHit[FilterStat_Max];
};

struct FilterFrameStatScope
{
	FilterFrameStatScope( FilterFrameStats& StatBlock, FilterStat Stat, bool ManualStart = false )
	{
		StatBlock.WasHit[Stat] = true;
		Target = &StatBlock.Stats[Stat];
		
		Stopped = false;

		if( !ManualStart )
		{
			Start();
		}
	}

	~FilterFrameStatScope()
	{
		if( !Stopped )
		{
			Stop();
		}
	}

	void Start()
	{
		StartTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	}

	void Stop()
	{
		int64_t End = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		*Target += (End-StartTime);

		Stopped = true;
	}

	int64_t* Target;
	int64_t StartTime;
	bool Stopped;
};

struct FilterFrameStatExcludeScope
{
	FilterFrameStatExcludeScope( FilterFrameStats& StatBlock, FilterStat Stat, bool ManualStart = false )
	{
		StatBlock.WasHit[Stat] = true;
		Target = &StatBlock.Stats[Stat];
		
		Stopped = false;

		if( !ManualStart )
		{
			Start();
		}
	}

	~FilterFrameStatExcludeScope()
	{
		if( !Stopped )
		{
			Stop();
		}
	}

	void Start()
	{
		StartTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	}

	void Stop()
	{
		int64_t End = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		*Target -= (End-StartTime);

		Stopped = true;
	}

	int64_t* Target;
	int64_t StartTime;
	bool Stopped;
};

struct SourceStats
{
	SourceStats()
	: PendingEssentialJobs( 0 )
	, PendingAIJobs( 0 )
	{
		Reset();
	}

	int64_t							LastTimestamp;
	int64_t							LastFrameIndex;
	int64_t							FrameCount;

	FilterFrameStats				Stats;

	// Queue profiling is kept separate from filter timings. Ingress latency is
	// measured from accepting a decoded frame until its first processing stage.
	uint64_t IngressFrames;
	uint64_t StartedFrames;
	uint64_t CoalescedFrames;
	uint64_t CoalescedAIFrames;
	uint64_t PendingEssentialJobs;
	uint64_t PendingAIJobs;
	uint64_t PeakPendingEssentialJobs;
	uint64_t PeakPendingAIJobs;
	uint64_t IngressQueueWaitSamples;
	uint64_t ContinuationQueueWaitSamples;
	uint64_t AIQueueWaitSamples;
	int64_t IngressQueueWaitTotalNS;
	int64_t IngressQueueWaitMaxNS;
	int64_t ContinuationQueueWaitTotalNS;
	int64_t ContinuationQueueWaitMaxNS;
	int64_t AIQueueWaitTotalNS;
	int64_t AIQueueWaitMaxNS;

	// Live queue gauges, populated when GetStats() takes its snapshot.
	int64_t OldestPendingEssentialAgeNS;
	int64_t OldestPendingAIAgeNS;
	int64_t ActiveJobAgeNS;
	bool ProcessingJobActive;
	bool AIReservationActive;
	uint64_t ActiveProcessingSources;
	uint64_t ActiveAISources;
	uint64_t ActiveBackgroundAIJobs;
	uint64_t MaximumConcurrentAIJobs;
	
	void Reset()
	{
		// Pending depths are live gauges and must survive a profiling reset.
		const uint64_t CurrentEssentialJobs = PendingEssentialJobs;
		const uint64_t CurrentAIJobs = PendingAIJobs;
		LastTimestamp = 0;
		LastFrameIndex = -1;
		FrameCount = 0;
		Stats.Reset();
		IngressFrames = 0;
		StartedFrames = 0;
		CoalescedFrames = 0;
		CoalescedAIFrames = 0;
		PendingEssentialJobs = CurrentEssentialJobs;
		PendingAIJobs = CurrentAIJobs;
		PeakPendingEssentialJobs = CurrentEssentialJobs;
		PeakPendingAIJobs = CurrentAIJobs;
		IngressQueueWaitSamples = 0;
		ContinuationQueueWaitSamples = 0;
		AIQueueWaitSamples = 0;
		IngressQueueWaitTotalNS = 0;
		IngressQueueWaitMaxNS = 0;
		ContinuationQueueWaitTotalNS = 0;
		ContinuationQueueWaitMaxNS = 0;
		AIQueueWaitTotalNS = 0;
		AIQueueWaitMaxNS = 0;
		OldestPendingEssentialAgeNS = 0;
		OldestPendingAIAgeNS = 0;
		ActiveJobAgeNS = 0;
		ProcessingJobActive = false;
		AIReservationActive = false;
		ActiveProcessingSources = 0;
		ActiveAISources = 0;
		ActiveBackgroundAIJobs = 0;
		MaximumConcurrentAIJobs = 0;
	}
};
