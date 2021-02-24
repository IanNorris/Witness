#pragma once

#include "cpprest/details/basic_types.h"
#include "GlobalContext.h"
#include "Message.h"
#include <vector>
#include <stdint.h>

using namespace std;
using namespace utility;

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

	vector<unsigned char> Jpeg;
};

struct CameraBeginMotionMessage : public Message
{
	CameraBeginMotionMessage( int CamIndex ) : MotionPercentage( 0.0 ), Camera( CamIndex ) {}

	ClipStatistics ClipStats;
	ClassificationResult Result;

	double MotionPercentage;
	int Camera;

	vector<unsigned char> Jpeg;
};

struct CameraUpdateMotionMessage : public Message
{
	CameraUpdateMotionMessage( int CamIndex ) : Camera( CamIndex ) {}

	ClipStatistics ClipStats;
	ClassificationResult Result;
	
	int Camera;

	vector<unsigned char> Jpeg;
};

struct CameraEndMotionMessage : public Message
{
	CameraEndMotionMessage( int CamIndex ) : Camera( CamIndex ) {}

	ClipStatistics ClipStats;
	ClassificationResult Result;

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
	CameraStopRecordMessage( int CamIndex, bool ManualStop ) : Camera( CamIndex ), ManualStop( ManualStop )  {}

	ClipStatistics ClipStats;

	int Camera;
	bool ManualStop;
};

struct CameraClipFinishedMessage : public Message
{
	CameraClipFinishedMessage( int CamIndex, bool ManualStop ) : Camera( CamIndex ), ManualStop( ManualStop )  {}

	ClipStatistics ClipStats;
	ClassificationResult Result;

	int Camera;
	bool ManualStop;
};

struct CameraStartupMessage : public Message
{
	CameraStartupMessage( int CamIndex ) : Camera( CamIndex ) {}

	int Camera;
};

struct CameraReconnectMessage : public Message
{
	CameraReconnectMessage( int CamIndex, string_t Error ) : Camera( CamIndex ), Error( Error ) {}

	int Camera;
	string_t Error;
};

struct CameraConnectedMessage : public Message
{
	CameraConnectedMessage( int CamIndex ) : Camera( CamIndex ) {}

	int Camera;
};

struct CameraWriteThumbnailMessage : public Message
{
	CameraWriteThumbnailMessage( int CamIndex ) : Camera( CamIndex ) {}

	vector<unsigned char> Jpeg;

	string_t Filename;

	int Camera;
};

struct CameraPreviewRequestMessage : public Message
{
	uint64_t LastLargePreviewTimestamp;
	uint64_t LastSmallPreviewTimestamp;
};

struct CameraAddedMessage : public Message
{
	CameraAddedMessage(int CamIndex) : Camera(CamIndex) {}

	int Camera;
};

struct CameraRemovedMessage : public Message
{
	CameraRemovedMessage(int CamIndex) : Camera(CamIndex) {}

	int Camera;
};