#pragma once

#include "Export.h"

#include <string>
#include <memory>

namespace Witness{
namespace Camera{

enum CameraStreamError
{
	CameraStreamError_Success,
	CameraStreamError_ConnectionError,
	CameraStreamError_NoStreams,
	CameraStreamError_NoH264Support,
	CameraStreamError_UnsupportedStreamFormat,
	CameraStreamError_FrameError,
	CameraStreamError_PacketError,
	CameraStreamError_DecoderReceiverError,
};

namespace Internal{
struct StreamData;
}

class CAMERA_API Stream
{
public:
	Stream( const std::string& StreamURL, int StreamIndex = 0 );
	virtual ~Stream();

	inline CameraStreamError GetError() { return Error; }

private:

	static void LogCallback( void* Data, int Level, const char* Format, va_list vargs );

	Internal::StreamData* InternalData;

	CameraStreamError Error;					
};

}}
