#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "RecordFilter.h"
#include "DebugBind.h"

#include <string>
#include <memory>

struct AVPacket;

namespace Witness{
namespace Camera{

CAMERA_API extern DebugConsole* TargetDebugConsole;

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
	ProcessingQueueFull,
	InvalidPacket,
	InvalidSetup,
	RefError,

	InternalError,
	UnknownError,
};

static const char* GetCameraStreamErrorMessage( CameraStreamError Error )
{
	switch( Error )
	{
	case CameraStreamError::Success:
		return "Success";

	case CameraStreamError::EndOfFile:
		return "End of file";

	case CameraStreamError::ConnectionError:
		return "Connection error";

	case CameraStreamError::NoStreams:
		return "No streams";

	case CameraStreamError::NoH264Support:
		return "No H264 support";

	case CameraStreamError::UnsupportedStreamFormat:
		return "Unsupported stream format";

	case CameraStreamError::UnsupportedStreamType:
		return "Unsupported stream type";

	case CameraStreamError::EncodeFailed:
		return "Encode failed";

	case CameraStreamError::FrameError:
		return "Frame error";

	case CameraStreamError::PacketError:
		return "Packet error";

	case CameraStreamError::DecoderReceiverError:
		return "Decoder receiver error";

	case CameraStreamError::EncoderCreationError:
		return "Encoder creation error";

	case CameraStreamError::FileNotWriteable:
		return "File not writable";

	case CameraStreamError::WriteFailed:
		return "Write failed";

	case CameraStreamError::NoStreamInput:
		return "Input stream is invalid";

	case CameraStreamError::ProcessingQueueFull:
		return "Frame processing queue is full. CPU is not powerful enough to handle current load.";

	case CameraStreamError::InvalidPacket:
		return "Invalid packet in buffer";

	case CameraStreamError::InternalError:
		return "Internal error";

	case CameraStreamError::InvalidSetup:
		return "One or more components of the setup struct were invalid.";

	case CameraStreamError::RefError:
		return "Failed to modify ref count for a packet.";

	case CameraStreamError::UnknownError:
	default:
		return "Unknown error";
	}
}

struct StreamData;

class CAMERA_API Stream : public Pimpl<StreamData>
{
public:
	Stream();
	virtual ~Stream();

	virtual CameraStreamError Initialize();
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream );
	virtual void Shutdown();

	virtual CameraStreamError WriteInterleavedPacket(const AVPacket* Packet) = 0;

	inline int GetErrorLine() { return m_LineNumber; }
	inline const char* GetFFMPEGErrorMessage() const { return m_ErrorMessage; }

protected:

	void OneTimeInit();

	static void LogCallback( void* Data, int Level, const char* Format, va_list vargs );

	
	char							m_ErrorMessage[256];
	int								m_LineNumber;
};

}}
