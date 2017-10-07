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
	CameraStreamError_UnsupportedStreamType,
	CameraStreamError_FrameError,
	CameraStreamError_PacketError,
	CameraStreamError_DecoderReceiverError,
	CameraStreamError_EncoderCreationError,
	CameraStreamError_FileNotWriteable,
	CameraStreamError_WriteFailed,


	CameraStreamError_InternalError,
	CameraStreamError_UnknownError,
};

struct StreamData;

class CAMERA_API Stream
{
public:
	Stream();
	virtual ~Stream();

	virtual CameraStreamError Initialize();
	virtual CameraStreamError ProcessFrame( Stream* TargetStream );
	virtual void Shutdown();

	inline int GetErrorLine() { return m_LineNumber; }
	inline StreamData* GetData() const { return m_InternalData; }

protected:

	void OneTimeInit();

	static void LogCallback( void* Data, int Level, const char* Format, va_list vargs );

	StreamData*						m_InternalData;
	int								m_LineNumber;
};

}}
