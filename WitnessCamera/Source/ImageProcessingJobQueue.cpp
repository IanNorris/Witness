#include "FFMPEG/Frame.h"
#include "FFMPEG/Common.h"
#include "ImageProcessingData.h"
#include "ImageProcessingJob.h"
#include "MotionFilter.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

#ifdef _WIN32
#include <windows.h>
#include <minmax.h>
#else
#include <algorithm>
using std::min;
using std::max;
#endif

namespace Witness{
namespace Camera{

PIMPL_CONSTRUCT(ImageProcessingJobQueueData)

const static int MaxCameras = 5;
const static int MaxCameraDelaySeconds = 1;
const static int ExpectedCameraFPS = 25;
const static int MaxQueueDepth = MaxCameras * MaxCameraDelaySeconds * ExpectedCameraFPS;

bool ImageProcessingJobQueue::Push(const SharedClassificationTask& Job, bool HighPriority)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	if( HighPriority )
	{
		if (ID.HighPriorityAsyncQueue.size() > MaxQueueDepth )
		{
			return false;
		}

		ID.HighPriorityAsyncQueue.push_back( Job );

		ID.Condition.notify_one();
	}
	else
	{
		if (ID.Queue.size() > MaxQueueDepth )
		{
			return false;
		}

		ID.Queue.push_back( Job );

		ID.Condition.notify_one();
	}

	return true;
}

bool ImageProcessingJobQueue::TryPop(SharedClassificationTask& Job)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	if( !ID.HighPriorityAsyncQueue.empty() )
	{
		auto Iter = ID.HighPriorityAsyncQueue.begin();
		while( Iter != ID.HighPriorityAsyncQueue.end() )
		{
			auto& JobRef = *Iter;

			bool ActiveJob = false;
			size_t Count = ID.ActiveSources.size();
			for( int Index = 0; Index < Count; Index++ )
			{
				if( ID.ActiveSources[Index] == JobRef->Frame.SourceID )
				{
					ActiveJob = true;
					break;
				}
			}

			if( !ActiveJob )
			{
				ID.ActiveSources.push_back( JobRef->Frame.SourceID );

				Job = *Iter;
				ID.HighPriorityAsyncQueue.erase( Iter );

				return true;
			}

			++Iter;
		}
	}

	if( !ID.Queue.empty() )
	{
		auto Iter = ID.Queue.begin();
		while( Iter != ID.Queue.end() )
		{
			auto& JobRef = *Iter;

			bool ActiveJob = false;
			size_t Count = ID.ActiveSources.size();
			for( int Index = 0; Index < Count; Index++ )
			{
				if( ID.ActiveSources[Index] == JobRef->Frame.SourceID )
				{
					ActiveJob = true;
					break;
				}
			}

			if( !ActiveJob )
			{
				ID.ActiveSources.push_back( JobRef->Frame.SourceID );

				Job = *Iter;
				ID.Queue.erase( Iter );

				return true;
			}

			++Iter;
		}
	}

	return false;
}

void ImageProcessingJobQueue::Pop(SharedClassificationTask& Job)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	while( !ID.WantExit )
	{
		while( ID.Queue.empty() && ID.HighPriorityAsyncQueue.empty() )
		{
			ID.Condition.wait( Lock );
		}

		if( !ID.HighPriorityAsyncQueue.empty() )
		{
			auto Iter = ID.HighPriorityAsyncQueue.begin();
			while( Iter != ID.HighPriorityAsyncQueue.end() )
			{
				auto& JobRef = *Iter;

				if (!JobRef)
				{
					ID.WantExit = true;
					break;
				}

				int CurrentSource = JobRef->Frame.SourceID;

				bool ActiveJob = false;
				size_t Count = ID.ActiveSources.size();
				for( int Index = 0; Index < Count; Index++ )
				{
					if( ID.ActiveSources[Index] == CurrentSource )
					{
						ActiveJob = true;
						break;
					}
				}

				if( !ActiveJob )
				{
					ID.ActiveSources.push_back( JobRef->Frame.SourceID );

					Job = *Iter;
					ID.HighPriorityAsyncQueue.erase( Iter );

					return;
				}

				++Iter;
			}
		}
		else
		{
			auto Iter = ID.Queue.begin();
			while( Iter != ID.Queue.end() )
			{
				auto& JobRef = *Iter;

				if (!JobRef)
				{
					ID.WantExit = true;
					break;
				}

				int CurrentSource = JobRef->Frame.SourceID;

				bool ActiveJob = false;
				size_t Count = ID.ActiveSources.size();
				for( int Index = 0; Index < Count; Index++ )
				{
					if( ID.ActiveSources[Index] == CurrentSource )
					{
						ActiveJob = true;
						break;
					}
				}

				if( !ActiveJob )
				{
					ID.ActiveSources.push_back( JobRef->Frame.SourceID );

					Job = *Iter;
					ID.Queue.erase( Iter );

					return;
				}

				++Iter;
			}
		}

		
		if (!ID.WantExit)
		{
			ID.Condition.wait(Lock);
		}
	}
}

void ImageProcessingJobQueue::RemoveAllForSource(int SourceID)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	for( auto Iter = ID.Queue.begin(); Iter != ID.Queue.end(); )
	{
		auto& IterRef = *Iter;
		if (IterRef->Frame.SourceID == SourceID)
		{
			Iter = ID.Queue.erase(Iter);
		}
		else
		{
			++Iter;
		}
	}
}

SourceStats ImageProcessingJobQueue::GetStats(int SourceID)
{
	auto& ID = *m_InternalData;
	return ID.GetStatsForSource(SourceID);
}

void ImageProcessingJobQueue::ResetStats(int SourceID)
{
	auto& ID = *m_InternalData;
	return ID.ResetStats(SourceID);
}

void ImageProcessingJobQueue::CompletedJob(int SourceID)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	for( auto Iter = ID.ActiveSources.begin(); Iter != ID.ActiveSources.end(); )
	{
		if (*Iter == SourceID)
		{
			ID.ActiveSources.erase(Iter);
			return;
		}

		++Iter;
	}
}

void ImageProcessingJobQueue::WorkerThreadMain()
{
	auto& ID = *m_InternalData;
	ID.WantExit = false;

	SharedClassificationTask Job;

	Pop(Job);

	if( Job )
	{
		uint64_t TaskTimestamp = Job->Frame.Timestamp;
		int TaskSourceID = Job->Frame.SourceID;

		if( Job->Next )
		{
			auto& ID = *m_InternalData;
			auto StateInternal = ID.GetStateForSource( TaskSourceID );
			
			Job->Frame.WantFullSizeOutput = StateInternal->HasViewerFullSize;
			Job->Frame.WantSmallOutput = StateInternal->HasViewerPreviewSize;

			Job->Next->DoWork( Job );

			StateInternal->HasViewerFullSize = Job->Frame.WantFullSizeOutput;
			StateInternal->HasViewerPreviewSize = Job->Frame.WantSmallOutput;
		}
		else
		{
			Job->FrameOwner->InputFrame->Unref();

			ID.AddFrame( TaskSourceID, TaskTimestamp, Job->Frame.Stats );
		}

		CompletedJob( TaskSourceID );
	}
}

void ImageProcessingJobQueue::RequestShutdown()
{
	auto& ID = *m_InternalData;
	ID.WantExit = true;
	MemoryBarrier();
	ID.Condition.notify_all();
}

void ImageProcessingJobQueueData::AddFrame(int Source, int64_t Timestamp, const FilterFrameStats& StatsIn )
{
	std::lock_guard<std::mutex> Lock(StatsMutex);

	auto& Ref = Stats[Source];

	Ref.FrameCount++;

	Ref.LastTimestamp = Timestamp;

	for( unsigned int Stat = 0; Stat < FilterStat_Max; Stat++ )
	{
		Ref.Stats.Stats[Stat] += StatsIn.Stats[Stat];
		Ref.Stats.FrameCount[Stat] += StatsIn.WasHit[Stat] ? 1 : 0;
	}
}

void ImageProcessingJobQueueData::ResetStats(int Source)
{
	std::lock_guard<std::mutex> Lock(StatsMutex);

	auto& Ref = Stats[Source];
	Ref.Reset();
}

SourceState::SourceState()
	: FrameContext( std::make_shared<FilterFrameContext>() )
	, HasViewerFullSize( false )
	, HasViewerPreviewSize( false )
{
}

SourceState::~SourceState()
{
	if (FrameContext && FrameContext->ConversionContext)
	{
		sws_freeContext( FrameContext->ConversionContext );
		FrameContext->ConversionContext = nullptr;
	}
}

}}
