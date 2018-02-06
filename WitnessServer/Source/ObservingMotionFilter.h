#pragma once

#include "Common.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

#include <cpprest/asyncrt_utils.h>

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

struct CameraBeginMotionMessage : public Message
{
	CameraBeginMotionMessage( int CamIndex ) : MotionPercentage( 0.0 ), Timestamp(0), Camera( CamIndex ) {}

	double MotionPercentage;
	uint64_t Timestamp;
	int Camera;

	vector<uchar> Jpeg;
};

struct CameraUpdateMotionMessage : public Message
{
	CameraUpdateMotionMessage( int CamIndex ) : TimestampStarted(0), TimestampNow(0), Camera( CamIndex ) {}

	uint64_t TimestampStarted;
	uint64_t TimestampNow;
	int Camera;

	vector<uchar> Jpeg;
};

struct CameraEndMotionMessage : public Message
{
	CameraEndMotionMessage( int CamIndex ) : TimestampStarted(0), TimestampNow(0), Camera( CamIndex ) {}

	uint64_t TimestampStarted;
	uint64_t TimestampNow;
	int Camera;
};

struct CameraStartRecordMessage : public Message
{
	CameraStartRecordMessage( int CamIndex, string_t PathIn ) : Path( PathIn ), Camera( CamIndex )  {}

	string_t Path;
	int Camera;
};

struct CameraStopRecordMessage : public Message
{
	CameraStopRecordMessage( int CamIndex ) : Camera( CamIndex )  {}

	int Camera;
};

class ObservingMotionFilter : public MotionFilter
{
public:

	enum class MotionState
	{
		None,
		Current,
		GracePeriod,
	};

	ObservingMotionFilter( const int CameraID, const shared_ptr<MessageBus>& MessageBusIn );
	virtual ~ObservingMotionFilter();

	virtual ClassificationResult FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager ) override;

	bool FlagToSaveNextFrame() { SaveNextFrame = true; }

private:

	shared_ptr<MessageBus>	MessageBusPtr;
	int						CameraID;
	int						FrameIndex;
	int						LastMotionIndex;
	bool					SaveNextFrame;

	double					LargestDelta;

	uint64_t				TimestampStarted;
	uint64_t				TimestampEnded;

	MotionState				State;
};
