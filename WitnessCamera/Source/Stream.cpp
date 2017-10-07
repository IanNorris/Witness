#include "InputStream.h"
#include "StreamData.h"

#include <windows.h>
#include <vector>
#include <iostream>

namespace Witness{
namespace Camera{

Stream::Stream()
: m_InternalData( new StreamData() )
{}

Stream::~Stream()
{
	Shutdown();

	if( m_InternalData->ConversionContext )
	{
		sws_freeContext( m_InternalData->ConversionContext );
		m_InternalData->ConversionContext = nullptr;
	}

	delete m_InternalData;
	m_InternalData = nullptr;
}

CameraStreamError Stream::Initialize()
{
	OneTimeInit();
	Shutdown();

	return CameraStreamError_Success;
}

CameraStreamError Stream::ProcessFrame( Stream* TargetStream )
{
	return CameraStreamError_Success;
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
	if( !m_InternalData->HasInitialized )
	{
		av_log_set_callback( &InputStream::LogCallback );
		av_register_all();
		avformat_network_init();

		m_InternalData->HasInitialized = true;
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
