#include "Stream.h"
#include "StreamData.h"

#include <windows.h>
#include <vector>
#include <iostream>

#define STREAM_ERROR( X )\
	m_LineNumber = __LINE__;\
	return X;

namespace Witness{
namespace Camera{

Stream::Stream( const std::string& StreamURL, int StreamIndex )
: m_InternalData( new StreamData() )
{
	m_InternalData->URL = StreamURL;
	m_InternalData->StreamIndex = StreamIndex;
}

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
	auto& ID = *m_InternalData;

	OneTimeInit();
	Shutdown();

	ID.FormatContext = avformat_alloc_context();
	
	av_dict_set( &ID.StreamOptions, "rtsp_transport", "tcp", 0 );
	
	if( avformat_open_input( &ID.FormatContext, ID.URL.c_str(), nullptr, &ID.StreamOptions ) != 0 )
	{
		STREAM_ERROR( CameraStreamError_ConnectionError );
	}

	if( avformat_find_stream_info( ID.FormatContext, nullptr ) < 0 )
	{
		STREAM_ERROR( CameraStreamError_NoStreams );
	}

	ID.ChosenStreamIndex = 0;
	for( unsigned int i = 0; i < ID.FormatContext->nb_streams; i++ )
	{
		if( ID.FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			if( ID.StreamIndex == i || ID.StreamIndex == 0 )
			{
				ID.ChosenStreamIndex = i;
				break;
			}
		}
	}

	av_init_packet( &ID.Packet );

	AVFormatContext* OutputFormat = avformat_alloc_context();
	
	av_read_play( ID.FormatContext );

	AVCodec* OutputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if( !OutputCodec )
	{
		STREAM_ERROR( CameraStreamError_NoH264Support );
	}

	ID.CodecContext = avcodec_alloc_context3( nullptr );
	avcodec_get_context_defaults3( ID.CodecContext, OutputCodec );
	avcodec_parameters_to_context( ID.CodecContext, ID.FormatContext->streams[ ID.ChosenStreamIndex ]->codecpar );
	
	if( avcodec_open2( ID.CodecContext, OutputCodec, nullptr ) < 0 )
	{
		STREAM_ERROR( CameraStreamError_UnsupportedStreamFormat );
	}

	AVPixelFormat OutputPixelFormat = AV_PIX_FMT_RGB24;
	unsigned int OutputWidth = ID.CodecContext->width;
	unsigned int OutputHeight = ID.CodecContext->height;

	AVPixelFormat InputPixelFormat = ID.CodecContext->pix_fmt;

	//Remap deprecated formats to avoid the warning output.
	switch(InputPixelFormat)
	{
	case AV_PIX_FMT_YUVJ420P:
		InputPixelFormat = AV_PIX_FMT_YUV420P;
		break;

	case AV_PIX_FMT_YUVJ422P:
		InputPixelFormat = AV_PIX_FMT_YUV422P;
		break;

	case AV_PIX_FMT_YUVJ444P:
		InputPixelFormat = AV_PIX_FMT_YUV444P;
		break;

	case AV_PIX_FMT_YUVJ440P:
		InputPixelFormat = AV_PIX_FMT_YUV440P;
		break;
	}

	ID.Output = std::make_unique<FFMPEG::Frame>( OutputWidth, OutputHeight, OutputPixelFormat );
	ID.Input = std::make_unique<FFMPEG::Frame>( ID.CodecContext->width, ID.CodecContext->height, InputPixelFormat );

	ID.ConversionContext = sws_getCachedContext(
		ID.ConversionContext,
		ID.Input->GetWidth(),
		ID.Input->GetHeight(),
		InputPixelFormat,
		OutputWidth,
		OutputHeight,
		OutputPixelFormat,
		SWS_BICUBIC,
		NULL,
		NULL,
		NULL );

	return CameraStreamError_Success;
}

CameraStreamError Stream::ProcessFrame()
{
	auto& ID = *m_InternalData;

	int Result;
	if( (Result = av_read_frame( m_InternalData->FormatContext, &ID.Packet )) < 0 )
	{
		STREAM_ERROR( CameraStreamError_FrameError );
	}

	if( ID.Packet.stream_index == ID.ChosenStreamIndex )
	{
		Result = avcodec_send_packet( m_InternalData->CodecContext, &ID.Packet );
		if( Result < 0 )
		{
			STREAM_ERROR( CameraStreamError_PacketError );
		}

		Result = 0;
		while( Result >= 0 )
		{
			Result = avcodec_receive_frame( m_InternalData->CodecContext, ID.Input->GetFrame() );
			if( Result == AVERROR(EAGAIN) || Result == AVERROR_EOF)
			{
				break;
			}
			else if( Result < 0 )
			{
				STREAM_ERROR( CameraStreamError_DecoderReceiverError );
			}
			else
			{
				int OutputSliceSize = sws_scale( m_InternalData->ConversionContext, ID.Input->GetFrame()->data, ID.Input->GetFrame()->linesize, 0, m_InternalData->CodecContext->height, ID.Output->GetFrame()->data, ID.Output->GetFrame()->linesize );

				unsigned char* ViewData = (unsigned char*)ID.Output->GetFrame()->data[0];

				ID.Input->Unref();
			}
		}
	}

	av_packet_unref( &ID.Packet );
	av_init_packet( &ID.Packet );

	return CameraStreamError_Success;
}

void Stream::Shutdown()
{
	auto& ID = *m_InternalData;

	ID.Input.reset();
	ID.Output.reset();

	if( ID.FormatContext )
	{
		avformat_close_input( &ID.FormatContext );
		avformat_free_context( ID.FormatContext );
		ID.FormatContext = nullptr;
	}

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
	if( !m_InternalData->HasInitialised )
	{
		av_log_set_callback( &Stream::LogCallback );
		av_register_all();
		avformat_network_init();

		m_InternalData->HasInitialised = true;
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