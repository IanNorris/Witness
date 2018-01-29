#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/opencv.hpp>

ObservingMotionFilter::ObservingMotionFilter( const int CameraID, const shared_ptr<MessageBus>& MessageBusIn )
: MotionFilter()
, MessageBusPtr( MessageBusIn )
, CameraID( CameraID )
, FrameIndex( 0 )
, SaveNextFrame( false )
{
}

ObservingMotionFilter::~ObservingMotionFilter()
{}

Witness::Camera::ClassificationResult ObservingMotionFilter::FilterFrame( unsigned int Width, unsigned int Height, void* Data, Witness::Camera::StreamManager* StreamManager )
{
	SaveNextFrame = true;

	if( SaveNextFrame )
	{
		SaveNextFrame = false;

		auto SaveFrameMessage = make_shared<CameraSnapshotMessage>( CameraID );

		cv::Mat RawData( cv::Size( Width, Height ), CV_8UC3, Data);

		cv::imencode( ".jpg", RawData, SaveFrameMessage->Jpeg, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, 70 } );

		MessageBusPtr->SendToClient( nullptr, SaveFrameMessage );
	}

	return MotionFilter::FilterFrame( Width, Height, Data, StreamManager );
}
