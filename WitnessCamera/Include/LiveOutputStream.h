#pragma once

#include "Stream.h"
#include "InputStream.h"
#include <mutex>
#include <chrono>
#include <string>

struct AVPacket;
struct AVRational;
struct AVFormatContext;
struct AVCodecContext;

namespace Witness{
namespace Camera{

struct LiveStreamSegment
{
	std::string FilePath;
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

private:

	CameraStreamError InitFormatContext();
	CameraStreamError StartNewSegment(const AVPacket* Packet);
	void FinishCurrentSegment();

	std::string _LiveCachePath;
	std::string _InitSegmentPath;

	std::vector<LiveStreamSegment>* _StreamBacklog;

	InputStream* _InputStream;

	// Single persistent format context for the entire live stream
	AVFormatContext* _FormatContext;
	AVCodecContext* _CodecContext;

	bool _HeaderWritten;
	bool _InitSegmentCaptured;
	bool _FirstPacketSeen;

	int64_t _SegmentStartDTS;
	double _CurrentSegmentDuration;

	const int _KeyframesPerSegment;
	int _SkipInitialKeyframes;

	int _CurrentSegmentIndex;

	std::chrono::time_point<std::chrono::system_clock> _CurrentSegmentWallTime;

	std::mutex* _SegmentsMutex;
};

}}
