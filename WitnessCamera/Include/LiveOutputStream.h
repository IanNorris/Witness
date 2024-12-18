#pragma once

#include "Stream.h"
#include "InputStream.h"
#include "OutputStream.h"
#include <mutex>
#include <chrono>

struct AVPacket;
struct AVRational;

namespace Witness{
namespace Camera{

struct LiveStreamSegment
{
	OutputStream* Stream;
	std::vector<OutputStream*> PartialStreams;
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

	void FinishStream();
	void FinishPartStream();
	CameraStreamError StartNewStream(const AVPacket* Packet);
	CameraStreamError StartNewPartStream(const AVPacket* Packet);

	const std::string* _LiveCachePath;

	std::vector<LiveStreamSegment>* _StreamBacklog;
	OutputStream* _CurrentStream;
	OutputStream* _CurrentPartStream;

	InputStream* _InputStream;

	const int _KeyframesPerSegment;
	int _KeyframesPerSegmentLeft;
	int _SkipInitialKeyframes;

	std::chrono::time_point<std::chrono::high_resolution_clock> m_LastSegmentTime;
	std::chrono::time_point<std::chrono::high_resolution_clock> m_LastPartTime;

	int _CurrentSegmentIndex;
	int _CurrentPartIndex;

	std::mutex* _SegmentsMutex;
};

}}
