#include "InputStream.h"
#include "StreamData.h"

#include <windows.h>
#include <vector>

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

PIMPL_CONSTRUCT(StreamData)

Stream::Stream()
: Pimpl()
{}

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

	size_t OriginalMessageSizeNeeded = std::vsnprintf(NULL, 0, Format, Args) + 1;
	std::vector<char> OriginalMessageBuf( OriginalMessageSizeNeeded );

	std::vsnprintf( OriginalMessageBuf.data(), OriginalMessageBuf.size(), Format, Args );

	AVClass* AVClassData = AVData ? *(AVClass**)AVData : nullptr;

	const char* OutputFormat = "%s: %s\n";

	size_t MessageSizeNeeded = std::snprintf(NULL, 0, OutputFormat, AVClassData ? AVClassData->item_name(AVData) : "Unknown", OriginalMessageBuf.data()) + 1;
	std::vector<char> MessageBuf( MessageSizeNeeded );

	std::snprintf( MessageBuf.data(), MessageBuf.size(), OutputFormat, AVClassData ? AVClassData->item_name(AVData) : "Unknown", OriginalMessageBuf.data());

	OutputDebugStringA( MessageBuf.data() );
}

}}
