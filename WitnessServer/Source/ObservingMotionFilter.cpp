#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/opencv.hpp>

const int ClipEndGracePeriodInSeconds = 10;
const int MaxFrameUpdatePeriodInSeconds = 3;
const int TargetThumbnailSize = 400;

ObservingMotionFilter::ObservingMotionFilter( const int CameraID, const shared_ptr<MessageBus>& MessageBusIn )
: MotionFilter()
, MessageBusPtr( MessageBusIn )
, CameraID( CameraID )
, FrameIndex( 0 )
, LastMotionIndex( INT_MIN )
, SaveNextFrame( false )
, TimestampStarted( 0 )
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

	auto Result = MotionFilter::FilterFrame( Width, Height, Data, StreamManager );
	
	uint64_t TimestampNow = datetime::utc_timestamp();

	if (Result.Importance > 0)
	{
		if( State == MotionState::None )
		{
			State = MotionState::Current;

			auto MotionMessage = make_shared<CameraBeginMotionMessage>( CameraID );

			TimestampStarted = MotionMessage->Timestamp = TimestampNow;

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
			}

			if (TimestampNow - TimestampStarted <= MaxFrameUpdatePeriodInSeconds)
			{
				auto MotionMessage = make_shared<CameraUpdateMotionMessage>( CameraID );

				MotionMessage->TimestampStarted = TimestampStarted;
				MotionMessage->TimestampNow = TimestampNow;

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
			TimestampEnded = TimestampNow;
			State = MotionState::GracePeriod;
		}
		else if( State == MotionState::GracePeriod )
		{
			if( TimestampNow - TimestampEnded >= ClipEndGracePeriodInSeconds )
			{
				State = MotionState::None;

				auto MotionMessage = make_shared<CameraEndMotionMessage>( CameraID );

				MotionMessage->TimestampStarted = TimestampStarted;
				MotionMessage->TimestampNow = TimestampNow;
				
				MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}

	return Result;
}
