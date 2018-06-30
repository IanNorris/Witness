#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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

Witness::Camera::ClassificationResult ObservingMotionFilter::FilterFrame( unsigned int Width, unsigned int Height, void* Data, Witness::Camera::StreamManager* StreamManager )
{
	FrameIndex++;

	SaveNextFrame = true;

	if( SaveNextFrame )
	{
		SaveNextFrame = false;

		auto SaveFrameMessage = make_shared<CameraSnapshotMessage>( CameraID );

		cv::Mat RawData( cv::Size( Width, Height ), CV_8UC3, Data);

		float Aspect = (float)RawData.cols / (float)RawData.rows;

		cv::Mat ResizedImage;
		resize( RawData, ResizedImage, cv::Size(TargetThumbnailSize,(int)((float)TargetThumbnailSize/Aspect)), 0, 0 );

		cv::imencode( ".jpg", ResizedImage, SaveFrameMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, 70 } );

		MessageBusPtr->SendToClient( nullptr, SaveFrameMessage );
	}

	ClassificationResult Result;
	try 
	{
		Result = MotionFilter::FilterFrame( Width, Height, Data, StreamManager );
	}
	catch (cv::Exception& e)
	{
		printf("OpenCV error: %s\n", e.what());
		abort();
	}
	
	uint64_t TimestampNow = datetime::utc_timestamp();

	if (Result.Importance > 0)
	{
		if( State == MotionState::None )
		{
			State = MotionState::Current;

			auto MotionMessage = make_shared<CameraBeginMotionMessage>( CameraID );

			ClipStats.TimestampClipStarted = MotionMessage->Timestamp = ClipStats.TimestampClipStarted > 0 ? min( ClipStats.TimestampClipStarted, TimestampNow ) : TimestampNow;
			ClipStats.TimestampMotionStarted = TimestampNow;
			ClipStats.TimestampMotionEnded = INT64_MIN;
			ClipStats.TimestampClipEnded = INT64_MIN;
			ClipStats.LargestMotionDelta = MotionMessage->MotionPercentage = Result.MotionPercentage;

			cv::Mat RawData( cv::Size( Width, Height ), CV_8UC3, Data);

			float Aspect = (float)RawData.cols / (float)RawData.rows;

			cv::Mat ResizedImage;
			resize( RawData, ResizedImage, cv::Size(TargetThumbnailSize,(int)((float)TargetThumbnailSize/Aspect)), 0, 0 );

			cv::imencode( ".jpg", ResizedImage, MotionMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, 70 } );

			MessageBusPtr->SendToClient( nullptr, MotionMessage );
		}
		else
		{
			if( State == MotionState::GracePeriod )
			{
				State = MotionState::Current;
				ClipStats.TimestampMotionEnded = 0;
				ClipStats.TimestampClipEnded = 0;
			}

			if( Result.MotionPercentage > ClipStats.LargestMotionDelta )
			{
				ClipStats.LargestMotionDelta = Result.MotionPercentage;

				auto MotionMessage = make_shared<CameraUpdateMotionMessage>( CameraID );

				MotionMessage->ClipStats = ClipStats;
				
				cv::Mat RawData( cv::Size( Width, Height ), CV_8UC3, Data);

				float Aspect = (float)RawData.cols / (float)RawData.rows;

				cv::Mat ResizedImage;
				resize( RawData, ResizedImage, cv::Size(TargetThumbnailSize,(int)((float)TargetThumbnailSize/Aspect)), 0, 0 );

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

	return Result;
}
