#pragma once

#include "Stream.h"
#include "InputStream.h"
#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <memory>

struct AVPacket;
struct AVRational;
struct AVFormatContext;
struct AVCodecContext;
struct AVIOContext;

namespace Witness{
namespace Camera{

typedef std::shared_ptr<std::vector<uint8_t>> SegmentBuffer;

struct LiveStreamSegment
{
	SegmentBuffer Data;
	double Duration;
	int SegmentIndex;
	std::chrono::time_point<std::chrono::system_clock> SegmentTime;
	bool Ready;
};

class CAMERA_API LiveOutputStream : public Stream
{
public:
	LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment);
	virtual ~LiveOutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet );

	int GetCurrentSegment()
	{
		return _CurrentSegmentIndex;
	}

	void GetSegments(std::vector<LiveStreamSegment>& OutSegments )
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		OutSegments = *_StreamBacklog;
	}

	SegmentBuffer GetInitSegment()
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		return _InitSegmentData;
	}

private:

	CameraStreamError InitFormatContext();
	CameraStreamError StartNewSegment(const AVPacket* Packet);
	void FinishCurrentSegment();

	void SetupMemoryIO();

	static int WriteBuffer(void* Opaque, const uint8_t* Buffer, int BufferSize);
	static int64_t SeekBuffer(void* Opaque, int64_t Offset, int Origin);

	std::string _LiveCachePath;

	std::vector<LiveStreamSegment>* _StreamBacklog;

	InputStream* _InputStream;

	// Single persistent format context for the entire live stream
	AVFormatContext* _FormatContext;
	AVIOContext* _AVIOContext;
	uint8_t* _AVIOBuffer;

	// Current write target — FFmpeg writes here via callbacks
	SegmentBuffer _CurrentBuffer;

	// Init segment stored in memory
	SegmentBuffer _InitSegmentData;

	bool _HeaderWritten;
	bool _InitSegmentCaptured;
	bool _HasInitialDTS;

	int64_t _InitialDTS;
	int64_t _LastWrittenDTS;
	int64_t _SegmentStartDTS;
	double _CurrentSegmentDuration;

	const int _KeyframesPerSegment;
	int _SkipInitialKeyframes;

	int _CurrentSegmentIndex;

	std::chrono::time_point<std::chrono::system_clock> _CurrentSegmentWallTime;

	std::mutex* _SegmentsMutex;
};

}}
