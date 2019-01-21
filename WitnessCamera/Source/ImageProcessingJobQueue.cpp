#include "FFMPEG/Frame.h"
#include "FFMPEG/Common.h"
#include "ImageProcessingData.h"
#include "ImageProcessingJob.h"
#include "MotionFilter.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

#include <minmax.h>

namespace Witness{
namespace Camera{

PIMPL_CONSTRUCT(ImageProcessingJobQueueData)

const static int MaxCameras = 5;
const static int MaxCameraDelaySeconds = 1;
const static int ExpectedCameraFPS = 25;
const static int MaxQueueDepth = MaxCameras * MaxCameraDelaySeconds * ExpectedCameraFPS;

bool ImageProcessingJobQueue::Push(const std::shared_ptr<ImageProcessingJob>& Job)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	if (ID.Queue.size() > MaxQueueDepth )
	{
		return false;
	}

	ID.Queue.push_back( Job );

	ID.Condition.notify_one();

	return true;
}

bool ImageProcessingJobQueue::TryPop(std::shared_ptr<ImageProcessingJob>& Job)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

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
				if( ID.ActiveSources[Index] == JobRef->SourceID )
				{
					ActiveJob = true;
					break;
				}
			}

			if( !ActiveJob )
			{
				ID.ActiveSources.push_back( JobRef->SourceID );

				Job = *Iter;
				ID.Queue.erase( Iter );

				return true;
			}

			++Iter;
		}
	}

	return false;
}

void ImageProcessingJobQueue::Pop(std::shared_ptr<ImageProcessingJob>& Job)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	while( true )
	{
		while( ID.Queue.empty() )
		{
			ID.Condition.wait( Lock );
		}

		auto Iter = ID.Queue.begin();
		while( Iter != ID.Queue.end() )
		{
			auto& JobRef = *Iter;
			int CurrentSource = JobRef->SourceID;

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
				ID.ActiveSources.push_back( JobRef->SourceID );

				Job = *Iter;
				ID.Queue.erase( Iter );

				return;
			}

			++Iter;
		}

		ID.Condition.wait( Lock );
	}
}

void ImageProcessingJobQueue::RemoveAllForSource(int SourceID)
{
	auto& ID = *m_InternalData;

	std::unique_lock<std::mutex> Lock( ID.QueueMutex );

	for( auto Iter = ID.Queue.begin(); Iter != ID.Queue.end(); )
	{
		auto& IterRef = *Iter;
		if (IterRef->SourceID == SourceID)
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

	std::shared_ptr<ImageProcessingJob> Job;

	Pop(Job);

	if( Job )
	{
		auto State = ID.GetStateForSource( Job->SourceID );

		{
			FilterFrameOwner Frame( Job->Frame, State->ConversionContext );

			FilterFrame InterfaceFrame = Frame.GetFilterFrame();

			FilterFrameStatScope Scope( InterfaceFrame.Stats, FilterStat_Process_Total );

			ClassificationResult FilterResult;
			{
				Job->Filter->ClassifyFrame( InterfaceFrame, FilterResult );
			}

			bool Used2P = false;

			/*if( FilterResult.ResultString )
			{
				Used2P = true;
				ClassificationResult ResultNew = Job->Filter->PostSuccessChildVisitor( OutputWidth, OutputHeight, Output->GetFrame()->data[0], nullptr );
				if( ResultNew.ResultString )
				{
					FilterResult = ResultNew;
				}
			}*/
		
			Job->Frame->Unref();

			//Conversion context can get created or updated if the size changes
			State->ConversionContext = Frame.ConversionContext;

			ID.AddFrame( Job->SourceID, Job->Timestamp,	Frame.Stats	);
		}

		CompletedJob( Job->SourceID );
	}
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
	: ConversionContext( nullptr )
{

}

SourceState::~SourceState()
{
	if (ConversionContext)
	{
		sws_freeContext( ConversionContext );
	}
}

}}
