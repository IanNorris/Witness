#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "RecordFilter.h"

#include <string>
#include <tchar.h>

namespace Witness{
namespace Camera{

enum class CameraStreamError
{
	Success,
	EndOfFile,
	ConnectionError,
	NoStreams,
	NoH264Support,
	UnsupportedStreamFormat,
	UnsupportedStreamType,
	EncodeFailed,
	FrameError,
	PacketError,
	DecoderReceiverError,
	EncoderCreationError,
	FileNotWriteable,
	WriteFailed,
	NoStreamInput,

	InternalError,
	UnknownError,
};

static const TCHAR* GetCameraStreamErrorMessage( CameraStreamError Error )
{
	switch( Error )
	{
	case CameraStreamError::Success:
		return _T("Success");

	case CameraStreamError::EndOfFile:
		return _T("End of file");

	case CameraStreamError::ConnectionError:
		return _T("Connection error");

	case CameraStreamError::NoStreams:
		return _T("No streams");

	case CameraStreamError::NoH264Support:
		return _T("No H264 support");

	case CameraStreamError::UnsupportedStreamFormat:
		return _T("Unsupported stream format");

	case CameraStreamError::UnsupportedStreamType:
		return _T("Unsupported stream type");

	case CameraStreamError::EncodeFailed:
		return _T("Encode failed");

	case CameraStreamError::FrameError:
		return _T("Frame error");

	case CameraStreamError::PacketError:
		return _T("Packet error");

	case CameraStreamError::DecoderReceiverError:
		return _T("Decoder receiver error");

	case CameraStreamError::EncoderCreationError:
		return _T("Encoder creation error");

	case CameraStreamError::FileNotWriteable:
		return _T("File not writable");

	case CameraStreamError::WriteFailed:
		return _T("Write failed");

	case CameraStreamError::NoStreamInput:
		return _T("Input stream is invalid");

	case CameraStreamError::InternalError:
		return _T("Internal error");

	case CameraStreamError::UnknownError:
	default:
		return _T("Unknown error");
	}
}

struct StreamData;

class CAMERA_API Stream : public Pimpl<StreamData>
{
public:
	Stream();
	virtual ~Stream();

	virtual CameraStreamError Initialize();
	virtual CameraStreamError ProcessFrame( IRecordFilter* Filter, Stream* TargetStream );
	virtual void Shutdown();

	inline int GetErrorLine() { return m_LineNumber; }
	inline const char* GetFFMPEGErrorMessage() const { return m_ErrorMessage; }

protected:

	void OneTimeInit();

	static void LogCallback( void* Data, int Level, const char* Format, va_list vargs );

	
	char							m_ErrorMessage[256];
	int								m_LineNumber;
};

}}
