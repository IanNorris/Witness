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

struct CAMERA_API FilterFrameStats
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
	{
		Reset();
	}

	int64_t							LastTimestamp;
	int64_t							LastFrameIndex;
	int64_t							FrameCount;

	FilterFrameStats				Stats;
	
	void Reset()
	{
		LastTimestamp = 0;
		LastFrameIndex = -1;
		FrameCount = 0;
		Stats.Reset();
	}
};
