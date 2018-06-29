#pragma once

#include "Stream.h"
#include "ImageProcessingJob.h"

struct AVRational;

namespace Witness{
namespace Camera{

typedef uint64_t (*UTCTimestampCallbackType)(void);

struct CAMERA_API InputStreamSetup
{
	InputStreamSetup()
		: GetTimestamp(nullptr)
		, MotionFilterFrameSkip(1)
		, MotionDetectFrameHeight( 720 )
		, MotionDetectThreshold( 0.1 )
		, HistoricalPacketBufferSeconds( 5.0 )
	{}

	bool Validate();

	UTCTimestampCallbackType GetTimestamp;

	unsigned int MotionFilterFrameSkip; //Accept only one in this number of frames. Eg 1 = full framerate, 2 = 1 in 2 frames, 4 = 1 in 4 frames.
	unsigned int MotionDetectFrameHeight;
	double MotionDetectThreshold;
	double HistoricalPacketBufferSeconds;
};

class CAMERA_API InputStream : public Stream
{
public:
	InputStream( const InputStreamSetup& Setup, int SourceID, ImageProcessingJobQueue* JobQueue, const std::string& StreamURL, int StreamIndex = 0 );
	virtual ~InputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream ) override;
	virtual void Shutdown() override;

	void GetTimebase( AVRational* TimebaseOut );
	void GetFramerate( AVRational* FramerateOut );

private:

	const static int64_t ConnectionTimeout = 5;

	static int InterruptCallback( void* Opaque );

	InputStreamSetup StreamSetup;

	ImageProcessingJobQueue* CommonJobQueue;

	StreamManager* m_StreamManager;

	int UniqueSourceID;
	int FrameIndex;
	int64_t TimeStarted;
	bool IsConnecting;
};

}}
