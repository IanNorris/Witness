#pragma once

#include "Stream.h"
#include "ImageProcessingJob.h"

#include <functional>

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
		, ExportMotionVectors( true )
	{}

	bool Validate();

	UTCTimestampCallbackType GetTimestamp;

	unsigned int MotionFilterFrameSkip; //Accept only one in this number of frames. Eg 1 = full framerate, 2 = 1 in 2 frames, 4 = 1 in 4 frames.
	unsigned int MotionDetectFrameHeight;
	double MotionDetectThreshold;
	double HistoricalPacketBufferSeconds;
	bool ExportMotionVectors;
};

class CAMERA_API InputStream : public Stream
{
public:

	struct CAMERA_API StreamStats
	{
		StreamStats()
		{
			Reset();
		}

		void Reset()
		{
			DecoderTimeTotal = 0;
			OutputTimeTotal = 0;
			ReadTimeTotal = 0;
			FrameCount = 0;
		}

		uint64_t DecoderTimeTotal;
		uint64_t OutputTimeTotal;
		uint64_t ReadTimeTotal;
		uint64_t FrameCount;
	};

	InputStream( const InputStreamSetup& Setup, int SourceID, ImageProcessingJobQueue* JobQueue, const std::string& StreamURL, int StreamIndex = 0 );
	virtual ~InputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	virtual CameraStreamError WriteInterleavedPacket(const AVPacket* Packet);

	void GetTimebase( AVRational* TimebaseOut );
	void GetFramerate( AVRational* FramerateOut );

	double GetFramerateDouble();

	// Optional callback invoked for every video packet (before unref).
	// Used by ContinuousOutputStream to receive packets without modifying the Stream interface.
	using PacketCallback = std::function<void(const AVPacket*)>;
	void SetPacketCallback(PacketCallback callback) { m_PacketCallback = std::move(callback); }

	StreamStats GetStats() { return Stats; }

	int GetSourceId() const { return UniqueSourceID; }

private:

	const static int64_t ConnectionTimeout = 5;

	static int InterruptCallback( void* Opaque );

	InputStreamSetup StreamSetup;
	StreamStats Stats;

	ImageProcessingJobQueue* CommonJobQueue;

	StreamManager* m_StreamManager;

	int UniqueSourceID;
	int FrameIndex;
	int64_t TimeStarted;
	bool IsConnecting;

	PacketCallback m_PacketCallback;
};

}}
