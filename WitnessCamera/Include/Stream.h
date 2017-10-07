#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "RecordFilter.h"

#include <string>

namespace Witness{
namespace Camera{

enum class CameraStreamError
{
	Success,
	ConnectionError,
	NoStreams,
	NoH264Support,
	UnsupportedStreamFormat,
	UnsupportedStreamType,
	FrameError,
	PacketError,
	DecoderReceiverError,
	EncoderCreationError,
	FileNotWriteable,
	WriteFailed,


	InternalError,
	UnknownError,
};

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

protected:

	void OneTimeInit();

	static void LogCallback( void* Data, int Level, const char* Format, va_list vargs );

	
	int								m_LineNumber;
};

}}
