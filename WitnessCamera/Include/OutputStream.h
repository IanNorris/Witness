#pragma once

#include "Stream.h"

struct AVPacket;
struct AVRational;

namespace Witness{
namespace Camera{

namespace FFMPEG{
class Frame;
}

class InputStream;

class CAMERA_API OutputStream : public Stream
{
public:
	OutputStream( const std::string& Path, InputStream * InputStream );
	OutputStream( const std::string& Path, unsigned int Width, unsigned int Height, int Framerate, bool IsBGR );
	virtual ~OutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet );
	CameraStreamError WriteFrame( FFMPEG::Frame* Frame );

	CameraStreamError CloseFile();

	int GetStreamIndex();

private:

	static int GlobalOutputStreamIndex;

	CameraStreamError SendAll( void );

	InputStream * m_InputStream;
	int FrameIndex;

	int StreamIndex;

	bool m_FileOpened;
};

}}
