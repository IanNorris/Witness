#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

const int ClipEndGracePeriodInSeconds = 10;
const int TargetThumbnailSize = 400;

ObservingMotionFilter::ObservingMotionFilter( double MotionThreshold, const char* MotionFilterName, const int CameraID, const shared_ptr<MessageBus>& MessageBusIn )
: MotionFilter( MotionThreshold, MotionFilterName )
, MessageBusPtr( MessageBusIn )
, CameraID( CameraID )
, FrameIndex( 0 )
, LastMotionIndex( INT_MIN )
, SaveNextFrame( false )
, State( MotionState::None )
{
}

ObservingMotionFilter::~ObservingMotionFilter()
{}

void ObservingMotionFilter::FilterFrame( ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame )
{
	FrameIndex++;

	SaveNextFrame = true;

	if( SaveNextFrame )
	{
		SaveNextFrame = false;

		auto SaveFrameMessage = make_shared<CameraSnapshotMessage>( CameraID );
		
		float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

		cv::Mat ResizedImage;
		resize( InputFrame, ResizedImage, cv::Size(TargetThumbnailSize,(int)((float)TargetThumbnailSize/Aspect)), 0, 0 );

		cv::imencode( ".jpg", ResizedImage, SaveFrameMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, 70 } );

		MessageBusPtr->SendToClient( nullptr, SaveFrameMessage );
	}

	try 
	{
		MotionFilter::FilterFrame( Result, InputFrame, GrayscaleInputFrame );
	}
	catch (cv::Exception& e)
	{
		printf("OpenCV error: %s\n", e.what());
		abort();
	}
	
	uint64_t TimestampNow = datetime::utc_timestamp();

	if (Result.ClassificationSuperset)
	{
		if( State == MotionState::None )
		{
			State = MotionState::Current;

			auto MotionMessage = make_shared<CameraBeginMotionMessage>( CameraID );

			ClipStats.TimestampClipStarted = MotionMessage->Timestamp = ClipStats.TimestampClipStarted > 0 ? min( ClipStats.TimestampClipStarted, TimestampNow ) : TimestampNow;
			ClipStats.TimestampMotionStarted = TimestampNow;
			ClipStats.TimestampMotionEnded = INT64_MIN;
			ClipStats.TimestampClipEnded = INT64_MIN;
			ClipStats.LargestMotionDelta = MotionMessage->MotionPercentage = Result.MotionAmount;
			
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
}
