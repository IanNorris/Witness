#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

const int ClipEndGracePeriodInSeconds = 10;
const int TargetLargeThumbnailSize = 720;
const int TargetThumbnailSize = 400;
const double PreviewTimeout = 3.0;

ObservingMotionFilter::ObservingMotionFilter( const shared_ptr<MotionChainNode>& MotionChain, const int CameraID, const shared_ptr<MessageBus>& MessageBusIn )
: MotionChain( MotionChain )
, MessageBusPtr( MessageBusIn )
, LastLargePreviewTimestamp(0)
, LastSmallPreviewTimestamp(0)
, CameraID( CameraID )
, FrameIndex( 0 )
, LastMotionIndex( INT_MIN )
, SaveNextFrame( false )
, State( MotionState::None )
{
}

ObservingMotionFilter::~ObservingMotionFilter()
{}

void ObservingMotionFilter::FilterFrame( const AVFrame* Frame, ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame )
{
	FrameIndex++;

	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;
	uint64_t Now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	bool SaveLarge = ((double)(Now - LastLargePreviewTimestamp) / NanoSecondsToSeconds) < PreviewTimeout;
	bool SaveSmall = ((double)(Now - LastSmallPreviewTimestamp) / NanoSecondsToSeconds) < PreviewTimeout;


	SaveNextFrame = SaveLarge | SaveSmall;

	try 
	{
		MotionChainNode* Next = MotionChain.get();
		while (Next)
		{
			unsigned int ClassificationSuperset = Result.ClassificationSuperset;

			Next->Filter->FilterFrame( Frame, Result, InputFrame, GrayscaleInputFrame );

			if (	(Result.ClassificationSuperset & Next->InclusiveFilter) != 0
				&&	(Result.ClassificationSuperset & Next->ExclusiveFilter) == 0
				&&	Result.MotionAmount >= Next->MinimumThreshold )
			{
				Next = Next->OnSuccess.get();
			}
			else
			{
				Result.ClassificationSuperset = ClassificationSuperset;
				Next = Next->OnFailure.get();
			}
		}
	}
	catch (cv::Exception& e)
	{
		printf("OpenCV error: %s\n", e.what());
		abort();
	}
	
	uint64_t TimestampNow = datetime::utc_timestamp();

	if (Result.ClassificationSuperset )
	{
		if( State == MotionState::None )
		{
			State = MotionState::Current;

			auto MotionMessage = make_shared<CameraBeginMotionMessage>( CameraID );

			ClipStats.TimestampClipStarted = TimestampNow;
			ClipStats.TimestampMotionStarted = TimestampNow;
			ClipStats.TimestampMotionEnded = INT64_MIN;
			ClipStats.TimestampClipEnded = INT64_MIN;
			ClipStats.LargestMotionDelta = MotionMessage->MotionPercentage = Result.MotionAmount;

			MotionMessage->ClipStats = ClipStats;
			
			float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

			cv::Mat ResizedImage;
			resize( InputFrame, ResizedImage, cv::Size(TargetThumbnailSize,(int)((float)TargetThumbnailSize/Aspect)), 0, 0 );

			cv::imencode( ".jpg", ResizedImage, MotionMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, 70 } );

			MessageBusPtr->SendToClient( nullptr, MotionMessage );
		}
		else
		{
			if( State == MotionState::GracePeriod )
			{
				State = MotionState::Current;
				ClipStats.TimestampMotionEnded = TimestampNow;
				ClipStats.TimestampClipEnded = TimestampNow;
			}

			if( Result.MotionAmount > ClipStats.LargestMotionDelta )
			{
				ClipStats.LargestMotionDelta = Result.MotionAmount;

				auto MotionMessage = make_shared<CameraUpdateMotionMessage>( CameraID );

				MotionMessage->ClipStats = ClipStats;
				
				float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

				cv::Mat ResizedImage;
				resize( InputFrame, ResizedImage, cv::Size(TargetThumbnailSize,(int)((float)TargetThumbnailSize/Aspect)), 0, 0 );

				cv::imencode( ".jpg", ResizedImage, MotionMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, 70 } );

				MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}
	else
	{
		if( State == MotionState::Current )
		{
			ClipStats.TimestampMotionEnded = TimestampNow;
			State = MotionState::GracePeriod;
		}
		else if( State == MotionState::GracePeriod )
		{
			if( TimestampNow - ClipStats.TimestampMotionEnded >= ClipEndGracePeriodInSeconds )
			{
				State = MotionState::None;

				auto MotionMessage = make_shared<CameraEndMotionMessage>( CameraID );

				ClipStats.TimestampClipEnded = TimestampNow;
				MotionMessage->ClipStats = ClipStats;
				
				MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}

	if( SaveNextFrame )
	{
		SaveNextFrame = false;

		auto SaveFrameMessage = make_shared<CameraSnapshotMessage>( CameraID );
		
		float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

		const int TargetSize = SaveLarge ? TargetLargeThumbnailSize : TargetThumbnailSize;
		const int Quality = SaveLarge ? 80 : 70;

		cv::Mat ResizedImage;
		resize( InputFrame, ResizedImage, cv::Size(TargetSize,(int)((float)TargetSize/Aspect)), 0, 0 );

		cv::imencode( ".jpg", ResizedImage, SaveFrameMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, Quality } );

		MessageBusPtr->SendToClient( nullptr, SaveFrameMessage );
	}
}
