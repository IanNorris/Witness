#include "FFMPEG/Frame.h"
#include "ImageProcessingData.h"
#include "ImageProcessingJob.h"
#include "MotionFilter.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

#include <minmax.h>

#include <windows.h> //MemorryBarrier

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
		auto Start = std::chrono::high_resolution_clock::now();

		auto State = ID.GetStateForSource( Job->SourceID );
		
		AVPixelFormat OutputPixelFormat = AV_PIX_FMT_BGR24;

		unsigned int OutputHeight = min( Job->TargetHeight, Job->Frame->GetHeight() );
		unsigned int OutputWidth = (int)(((float)Job->Frame->GetWidth() / (float)Job->Frame->GetHeight()) * (float)OutputHeight);

		OutputHeight &= (~15);
		OutputWidth &= (~15);

		AVPixelFormat InputPixelFormat = (AVPixelFormat)Job->Frame->GetFormat();

		//Remap deprecated formats to avoid the warning output.
		switch(InputPixelFormat)
		{
		case AV_PIX_FMT_YUVJ420P:
			InputPixelFormat = AV_PIX_FMT_YUV420P;
			break;

		case AV_PIX_FMT_YUVJ422P:
			InputPixelFormat = AV_PIX_FMT_YUV422P;
			break;

		case AV_PIX_FMT_YUVJ444P:
			InputPixelFormat = AV_PIX_FMT_YUV444P;
			break;

		case AV_PIX_FMT_YUVJ440P:
			InputPixelFormat = AV_PIX_FMT_YUV440P;
			break;
		}


		auto Output = std::make_shared<FFMPEG::Frame>( OutputWidth, OutputHeight, OutputPixelFormat );
		Output->Prepare();

		//int ScaleMethod = SWS_BICUBIC;
		int ScaleMethod = SWS_FAST_BILINEAR;

		State->ConversionContext = sws_getCachedContext(
			State->ConversionContext,
			Job->Frame->GetWidth(),
			Job->Frame->GetHeight(),
			InputPixelFormat,
			OutputWidth,
			OutputHeight,
			OutputPixelFormat,
			ScaleMethod,
			NULL,
			NULL,
			NULL );

		sws_scale( State->ConversionContext, Job->Frame->GetFrame()->data, Job->Frame->GetFrame()->linesize, 0, Job->Frame->GetHeight(), Output->GetFrame()->data, Output->GetFrame()->linesize );

		cv::Mat MotionFrame( cv::Size( OutputWidth, OutputHeight ), CV_8UC3, Output->GetFrame()->data[0] );
		cv::Mat MotionFrameGray;

	    cvtColor( MotionFrame, MotionFrameGray, CV_RGB2GRAY );

		MemoryBarrier();

		auto AfterScale = std::chrono::high_resolution_clock::now();

		ClassificationResult FilterResult;
		Job->Filter->FilterFrame( Job->Frame->GetFrame(), FilterResult, MotionFrame, MotionFrameGray );

		bool Used2P = false;
		auto AfterMD = std::chrono::high_resolution_clock::now();

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

		auto End = std::chrono::high_resolution_clock::now();

		ID.AddFrame( 
			Job->SourceID, 
			Job->Timestamp,
			AfterScale.time_since_epoch().count() - Start.time_since_epoch().count(),
			AfterMD.time_since_epoch().count() - AfterScale.time_since_epoch().count(),
			Used2P ? (End.time_since_epoch().count() - AfterMD.time_since_epoch().count()) : 0
		);

		CompletedJob( Job->SourceID );
	}
}

void ImageProcessingJobQueueData::AddFrame(int Source, int64_t Timestamp, int64_t ScaleProcessingTime, int64_t MDProcessingTime, int64_t SecondPassProcessingTime)
{
	std::lock_guard<std::mutex> Lock(StatsMutex);

	auto& Ref = Stats[Source];

	Ref.FrameCount++;

	if (SecondPassProcessingTime > 0)
	{
		Ref.SecondPassFrameCount++;
	}

	Ref.LastTimestamp = Timestamp;
	Ref.ScaleTotalProcessingTime += ScaleProcessingTime;
	Ref.MotionDetectionTotalProcessingTime += MDProcessingTime;
	Ref.SecondPassFilterTotalProcessingTime += SecondPassProcessingTime;
	Ref.TotalProcessingTime += ScaleProcessingTime + MDProcessingTime + SecondPassProcessingTime;

	/*if (Ref.FrameCount % 1000 == 999)
	{
		double Total = (double)Ref.TotalProcessingTime / ((double)Ref.FrameCount * 1000.0 * 1000.0);
		double Scale = (double)Ref.ScaleTotalProcessingTime / ((double)Ref.FrameCount * 1000.0 * 1000.0);
		double MD = (double)Ref.MotionDetectionTotalProcessingTime / ((double)Ref.FrameCount * 1000.0 * 1000.0);
		double SP = (double)Ref.SecondPassFilterTotalProcessingTime / ((double)Ref.SecondPassFrameCount * 1000.0 * 1000.0);
		printf("Source %d: Total %.2fms, Scale: %.2fms, MD: %.2fms, 2p: %.2fms\n", Source, (float)Total, (float)Scale, (float)MD, (float)SP );
	}*/
}

void ImageProcessingJobQueueData::ResetStats(int Source)
{
	std::lock_guard<std::mutex> Lock(StatsMutex);

	auto& Ref = Stats[Source];

	Ref.FrameCount = 0;
	Ref.LastTimestamp = 0;
	Ref.ScaleTotalProcessingTime = 0;
	Ref.MotionDetectionTotalProcessingTime = 0;
	Ref.SecondPassFilterTotalProcessingTime = 0;
	Ref.TotalProcessingTime = 0;
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
