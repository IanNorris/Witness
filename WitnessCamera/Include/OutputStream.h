#pragma once

#include "Stream.h"

struct AVPacket;
struct AVRational;

namespace Witness{
namespace Camera{

namespace FFMPEG{
class Frame;
class InMemoryIOContext;
}

class InputStream;

class CAMERA_API OutputStream : public Stream
{
public:
	OutputStream( const std::string& Path, InputStream * InputStream, bool InMemory );
	OutputStream( const std::string& Path, unsigned int Width, unsigned int Height, int Framerate, bool IsBGR );
	virtual ~OutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet );
	CameraStreamError WriteFrame( FFMPEG::Frame* Frame );

	CameraStreamError CloseFile();

	int GetStreamIndex();

	FFMPEG::InMemoryIOContext* GetOutput() { return m_IOContext; }

private:

	static int GlobalOutputStreamIndex;

	CameraStreamError SendAll( void );

	InputStream * m_InputStream;
	FFMPEG::InMemoryIOContext* m_IOContext;
	int FrameIndex;

	int StreamIndex;

	bool m_FileOpened;
	bool m_InMemory;
};

}}
