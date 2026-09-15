#include "InputStream.h"
#include "StreamData.h"

#include <windows.h>
#include <vector>
#include <Log.h>

namespace
{
	thread_local int FFmpegLogSourceID = -1;
	thread_local const char* FFmpegLogPhase = "unscoped";
	thread_local uint64_t FFmpegLogErrorCount = 0;
}

void FFMPEGErrorToString(int ErrorCode, char* Buffer, size_t BufferSize)
{
	if (ErrorCode == 0)
	{
		strcpy_s(Buffer, BufferSize, "");
	}
	else if (av_strerror(ErrorCode, Buffer, BufferSize) < 0)
	{
		strcpy_s(Buffer, BufferSize, "Unknown error");
	}
}

namespace Witness{
namespace Camera{

FFmpegLogContextScope::FFmpegLogContextScope( int SourceID, const char* Phase )
	: PreviousSourceID( FFmpegLogSourceID )
	, PreviousPhase( FFmpegLogPhase )
	, StartingErrorCount( FFmpegLogErrorCount )
{
	FFmpegLogSourceID = SourceID;
	FFmpegLogPhase = Phase ? Phase : "unknown";
}

FFmpegLogContextScope::~FFmpegLogContextScope()
{
	FFmpegLogSourceID = PreviousSourceID;
	FFmpegLogPhase = PreviousPhase;
}

bool FFmpegLogContextScope::HasError() const
{
	return FFmpegLogErrorCount != StartingErrorCount;
}

PIMPL_CONSTRUCT(StreamData)

Stream::Stream()
: Pimpl()
{
	m_ErrorMessage[0] = '\0';
	m_LineNumber = 0;
}

Stream::~Stream()
{
	Shutdown();

	if( m_InternalData->ConversionContext )
	{
		sws_freeContext( m_InternalData->ConversionContext );
		m_InternalData->ConversionContext = nullptr;
	}
}

CameraStreamError Stream::Initialize()
{
	OneTimeInit();
	Shutdown();

	return CameraStreamError::Success;
}

CameraStreamError Stream::ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream)
{
	return CameraStreamError::Success;
}

void Stream::Shutdown()
{
	auto& ID = *m_InternalData;

	ID.Input.reset();
	ID.Output.reset();

	if( ID.CodecContext )
	{
		avcodec_free_context( &ID.CodecContext );
		ID.CodecContext = nullptr;
	}

	if( ID.StreamOptions )
	{
		av_dict_free( &ID.StreamOptions );
		ID.StreamOptions = nullptr;
	}
}

void Stream::OneTimeInit()
{
	if( !m_InternalData->HasOneTimeInitialized )
	{
		av_log_set_callback( &InputStream::LogCallback );
		avformat_network_init();

		m_InternalData->HasOneTimeInitialized = true;
	}
}

void Stream::LogCallback( void* AVData, int Level, const char* Format, va_list Args )
{
	if( Level > AV_LOG_WARNING )
	{
		return;
	}

	va_list SizeArgs;
	va_copy( SizeArgs, Args );
	int OriginalMessageLength = std::vsnprintf(NULL, 0, Format, SizeArgs);
	va_end( SizeArgs );
	if( OriginalMessageLength < 0 )
		return;
	std::vector<char> OriginalMessageBuf( (size_t)OriginalMessageLength + 1 );

	va_list MessageArgs;
	va_copy( MessageArgs, Args );
	std::vsnprintf( OriginalMessageBuf.data(), OriginalMessageBuf.size(), Format, MessageArgs );
	va_end( MessageArgs );
	while( OriginalMessageLength > 0 &&
		(OriginalMessageBuf[OriginalMessageLength - 1] == '\n' || OriginalMessageBuf[OriginalMessageLength - 1] == '\r') )
	{
		OriginalMessageBuf[--OriginalMessageLength] = '\0';
	}

	AVClass* AVClassData = AVData ? *(AVClass**)AVData : nullptr;

	const char* OutputFormat = "%s: %s\n";

	size_t MessageSizeNeeded = std::snprintf(NULL, 0, OutputFormat, AVClassData ? AVClassData->item_name(AVData) : "Unknown", OriginalMessageBuf.data()) + 1;
	std::vector<char> MessageBuf( MessageSizeNeeded );

	std::snprintf( MessageBuf.data(), MessageBuf.size(), OutputFormat, AVClassData ? AVClassData->item_name(AVData) : "Unknown", OriginalMessageBuf.data());

	OutputDebugStringA( MessageBuf.data() );

	if( Level <= AV_LOG_ERROR )
	{
		++FFmpegLogErrorCount;
		LOG_ERROR( "[FFmpeg] Camera %d %s (%s): %s", FFmpegLogSourceID,
			FFmpegLogPhase, AVClassData ? AVClassData->item_name(AVData) : "unknown",
			OriginalMessageBuf.data() );
	}
}

}}
