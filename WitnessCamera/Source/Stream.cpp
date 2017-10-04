#include "Stream.h"
#include "StreamData.h"

#include <windows.h>
#include <vector>
#include <iostream>

namespace Witness{
namespace Camera{

Stream::Stream( const std::string& StreamURL, int StreamIndex )
: InternalData( new Internal::StreamData() )
, Error( CameraStreamError_Success )
{
	InternalData->StreamOptions = nullptr;
	InternalData->ConversionContext = nullptr;
	InternalData->FormatContext = avformat_alloc_context();
	InternalData->CodecContext = avcodec_alloc_context3(nullptr);

	av_log_set_callback(&Stream::LogCallback);
	av_register_all();
	avformat_network_init();

	av_dict_set( &InternalData->StreamOptions, "rtsp_transport", "tcp", 0 );
	
	if( avformat_open_input( &InternalData->FormatContext, StreamURL.c_str(), nullptr, &InternalData->StreamOptions ) != 0 )
	{
		Error = CameraStreamError_ConnectionError;
		return;
	}

	if( avformat_find_stream_info( InternalData->FormatContext, nullptr ) < 0 )
	{
		Error = CameraStreamError_NoStreams;
		return;
	}

	unsigned int ChosenStream = 0;
	for( unsigned int i = 0; i < InternalData->FormatContext->nb_streams; i++ )
	{
		if( InternalData->FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			if( StreamIndex == i || StreamIndex == 0 )
			{
				ChosenStream = i;
				break;
			}
		}
	}

	AVPacket Packet;
	av_init_packet( &Packet );

	AVFormatContext* OutputFormat = avformat_alloc_context();
	
	av_read_play( InternalData->FormatContext );

	AVCodec* OutputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if( !OutputCodec )
	{
		Error = CameraStreamError_NoH264Support;
		return;
	}

	avcodec_get_context_defaults3( InternalData->CodecContext, OutputCodec );

	avcodec_parameters_to_context( InternalData->CodecContext, InternalData->FormatContext->streams[ChosenStream]->codecpar );
	
	if( avcodec_open2( InternalData->CodecContext, OutputCodec, nullptr ) < 0 )
	{
		Error = CameraStreamError_UnsupportedStreamFormat;
		return;
	}

	unsigned int OutputWidth = InternalData->CodecContext->width;
	unsigned int OutputHeight = InternalData->CodecContext->height;

	InternalData->ConversionContext = sws_getContext( 
		InternalData->CodecContext->width,
		InternalData->CodecContext->height,
		InternalData->CodecContext->pix_fmt, 
		OutputWidth,
		OutputHeight,
		AV_PIX_FMT_RGB24,
		SWS_BICUBIC,
		NULL,
		NULL,
		NULL );

	AVFrame* PicOut = av_frame_alloc();
	size_t PicSizeOut = av_image_get_buffer_size( AV_PIX_FMT_RGB24, OutputWidth, OutputHeight, 1 );
	uint8_t* PicBufOut = (uint8_t*)av_malloc( PicSizeOut );

	av_image_fill_arrays( PicOut->data, PicOut->linesize, PicBufOut, AV_PIX_FMT_RGB24, OutputWidth, OutputHeight, 1 );

	AVFrame* PicIn = av_frame_alloc();
	size_t PicSizeIn = av_image_get_buffer_size( InternalData->CodecContext->pix_fmt, InternalData->CodecContext->width, InternalData->CodecContext->height, 1 );
	uint8_t* PicBufIn = (uint8_t*)av_malloc( PicSizeIn );

	av_image_fill_arrays( PicIn->data, PicIn->linesize, PicBufIn, InternalData->CodecContext->pix_fmt, InternalData->CodecContext->width, InternalData->CodecContext->height, 1 );
	
	while( true )
	{
		int result;
		if( (result = av_read_frame( InternalData->FormatContext, &Packet )) < 0 )
		{
			Error = CameraStreamError_FrameError;
			return;
		}

		if( Packet.stream_index == ChosenStream )
		{
			result = avcodec_send_packet( InternalData->CodecContext, &Packet );
			if( result < 0 )
			{
				Error = CameraStreamError_PacketError;
				return;
			}

			result = 0;
			while( result >= 0 )
			{
				
				result = avcodec_receive_frame( InternalData->CodecContext, PicIn );
				if( result == AVERROR(EAGAIN) || result == AVERROR_EOF)
				{
					break;
				}
				else if( result < 0 )
				{
					Error = CameraStreamError_DecoderReceiverError;
					return;
				}
				else
				{
					int OutputSliceSize = sws_scale( InternalData->ConversionContext, PicIn->data, PicIn->linesize, 0, InternalData->CodecContext->height, PicOut->data, PicOut->linesize );

					unsigned char* ViewData = (unsigned char*)PicOut->data[0];

					av_frame_unref( PicIn );
				}
			}
		}

		av_packet_unref( &Packet );
		av_init_packet( &Packet );

		Sleep(10);
		
	}
}

Stream::~Stream()
{
	delete InternalData;
	InternalData = nullptr;
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