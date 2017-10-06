#pragma once

#include "Export.h"

#include <string>

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


	CameraStreamError_InternalError,
};

struct StreamData;

class CAMERA_API Stream
{
public:
	Stream();
	virtual ~Stream();

	virtual CameraStreamError Initialize();
	virtual CameraStreamError ProcessFrame();
	virtual void Shutdown();

	inline int GetErrorLine() { return m_LineNumber; }

protected:

	void OneTimeInit();

	static void LogCallback( void* Data, int Level, const char* Format, va_list vargs );

	StreamData*						m_InternalData;
	int								m_LineNumber;
};

}}
