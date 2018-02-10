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

struct ClipStatistics
{
	uint64_t				TimestampClipStarted;
	uint64_t				TimestampMotionStarted;
	uint64_t				TimestampClipEnded;
	uint64_t				TimestampMotionEnded;
	double					LargestMotionDelta;

	ClipStatistics() { Clear(); }

	void Clear()
	{
		TimestampClipStarted = INT64_MAX;
		TimestampMotionStarted = INT64_MAX;
		TimestampMotionEnded = 0;
		TimestampClipEnded = 0;
		LargestMotionDelta = 0.0;
	}
};

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
	CameraUpdateMotionMessage( int CamIndex ) : Camera( CamIndex ) {}

	ClipStatistics ClipStats;
	
	int Camera;

	vector<uchar> Jpeg;
};

struct CameraEndMotionMessage : public Message
{
	CameraEndMotionMessage( int CamIndex ) : Camera( CamIndex ) {}

	ClipStatistics ClipStats;

	int Camera;
};

struct CameraStartRecordMessage : public Message
{
	CameraStartRecordMessage( int CamIndex, int64_t TimestampIn, string_t PathIn ) 
	: Path( PathIn )
	, Timestamp( TimestampIn )
	, Camera( CamIndex ) 
	{}

	string_t Path;
	int64_t Timestamp;
	int Camera;
};

struct CameraStopRecordMessage : public Message
{
	CameraStopRecordMessage( int CamIndex ) : Camera( CamIndex )  {}

	ClipStatistics ClipStats;

	int Camera;
};

struct CameraClipFinishedMessage : public Message
{
	CameraClipFinishedMessage( int CamIndex ) : Camera( CamIndex )  {}

	ClipStatistics ClipStats;

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

	const ClipStatistics& GetClipStatistics() const { return ClipStats; }

	void SetManualClipStart( uint64_t ClipStart ) { ClipStats.TimestampClipStarted = min( ClipStart, ClipStats.TimestampClipStarted ); }
	void SetManualClipEnd( uint64_t ClipEnd ) { ClipStats.TimestampClipEnded = max( ClipEnd, ClipStats.TimestampClipEnded ); }

	void ClearStats() { ClipStats.Clear(); }

private:

	shared_ptr<MessageBus>	MessageBusPtr;
	int						CameraID;
	int						FrameIndex;
	int						LastMotionIndex;
	bool					SaveNextFrame;

	ClipStatistics			ClipStats;

	MotionState				State;
};
