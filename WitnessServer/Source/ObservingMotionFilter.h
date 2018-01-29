#pragma once

#include "Common.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

#include <opencv2/opencv.hpp>

#include "MessageBus.h"

class MessageBus;

using namespace Witness::Camera;

struct CameraSnapshotMessage : public Message
{
	CameraSnapshotMessage( int CamIndex ) : Camera( CamIndex ) {}

	int Camera;

	vector<uchar> Jpeg;
};

class ObservingMotionFilter : public MotionFilter
{
public:

	ObservingMotionFilter( const int CameraID, const shared_ptr<MessageBus>& MessageBusIn );
	virtual ~ObservingMotionFilter();

	virtual ClassificationResult FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager ) override;

	bool FlagToSaveNextFrame() { SaveNextFrame = true; }

private:

	shared_ptr<MessageBus>	MessageBusPtr;
	int						CameraID;
	int						FrameIndex;
	bool					SaveNextFrame;
};
