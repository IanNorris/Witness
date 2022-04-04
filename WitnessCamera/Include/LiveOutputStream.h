#pragma once

#include "Stream.h"
#include "InputStream.h"
#include "OutputStream.h"

struct AVPacket;
struct AVRational;

namespace Witness{
namespace Camera{

class CAMERA_API LiveOutputStream : public Stream
{
public:
	LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment);
	virtual ~LiveOutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet );

private:

	void FinishStream();
	CameraStreamError StartNewStream(const AVPacket* Packet);

	const std::string* _LiveCachePath;

	std::vector<OutputStream*>* _StreamBacklog;
	OutputStream* _CurrentStream;

	InputStream* _InputStream;

	const int _KeyframesPerSegment;
	int _KeyframesPerSegmentLeft;
	int _SkipInitialKeyframes;
};

}}
