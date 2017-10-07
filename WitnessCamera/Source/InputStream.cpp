#include "InputStream.h"
#include "OutputStream.h"
#include "StreamData.h"

#include <windows.h>
#include <vector>
#include <iostream>

namespace Witness{
namespace Camera{

InputStream::InputStream( const std::string& StreamURL, int StreamIndex )
: Stream()
{
	m_InternalData->Path = StreamURL;
	m_InternalData->StreamIndex = StreamIndex;
}

InputStream::~InputStream()
{}

CameraStreamError InputStream::Initialize()
{
	CameraStreamError StreamInitResult = Stream::Initialize();
	if( StreamInitResult != CameraStreamError_Success )
	{
		return StreamInitResult;
	}

	auto& ID = *m_InternalData;

	ID.FormatContext = avformat_alloc_context();
	
	av_dict_set( &ID.StreamOptions, "rtsp_transport", "tcp", 0 );
	
	if( avformat_open_input( &ID.FormatContext, ID.Path.c_str(), nullptr, &ID.StreamOptions ) != 0 )
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

CameraStreamError InputStream::ProcessFrame( Stream* TargetStream )
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
				Stream* Output = dynamic_cast<OutputStream*>(TargetStream);
				if( Output )
				{
					

					av_packet_rescale_ts( 
						&ID.Packet, 
						ID.FormatContext->streams[ID.ChosenStreamIndex]->time_base,
						Output->GetData()->FormatContext->streams[ID.ChosenStreamIndex]->time_base );

					Result = av_interleaved_write_frame( Output->GetData()->FormatContext, &ID.Packet );
					if( Result < 0 )
					{
						STREAM_ERROR( CameraStreamError_WriteFailed );
					}
				}

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

void InputStream::Shutdown()
{
	auto& ID = *m_InternalData;

	if( ID.FormatContext )
	{
		avformat_close_input( &ID.FormatContext );
		avformat_free_context( ID.FormatContext );
		ID.FormatContext = nullptr;
	}

	Stream::Shutdown();
}

}}
