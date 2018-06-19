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
	OutputStream( const std::string& Path, InputStream * InputStream = nullptr );
	OutputStream( const std::string& Path, unsigned int Width, unsigned int Height, int Framerate, bool IsBGR );
	virtual ~OutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( AVRational* TimeBase, AVPacket* Packet );
	CameraStreamError WriteFrame( FFMPEG::Frame* Frame );

	CameraStreamError CloseFile();

private:

	CameraStreamError SendAll( void );

	InputStream * m_InputStream;
	int FrameIndex;
};

}}
