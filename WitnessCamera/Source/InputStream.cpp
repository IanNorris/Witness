#include "InputStream.h"
#include "OutputStream.h"
#include "StreamManager.h"
#include "StreamData.h"

#include <vector>
#include <iostream>
#include <chrono>

namespace Witness{
namespace Camera{

InputStream::InputStream( const std::string& StreamURL, int StreamIndex )
: Stream()
, m_StreamManager( new StreamManager() )
, TimeStarted( 0 )
, IsConnecting( false )
{
	m_InternalData->Path = StreamURL;
	m_InternalData->StreamIndex = StreamIndex;
}

InputStream::~InputStream()
{
	delete m_StreamManager;
	m_StreamManager = nullptr;
}

CameraStreamError InputStream::Initialize()
{
	if( m_InternalData->HasInitialized )
	{
		return CameraStreamError::Success;
	}

	m_InternalData->HasInitialized = true;

	CameraStreamError StreamInitResult = Stream::Initialize();
	if( StreamInitResult != CameraStreamError::Success )
	{
		return StreamInitResult;
	}

	auto& ID = *m_InternalData;

	ID.FormatContext = avformat_alloc_context();
	ID.FormatContext->interrupt_callback.callback = InputStream::InterruptCallback;
	ID.FormatContext->interrupt_callback.opaque = this;

	
	av_dict_set( &ID.StreamOptions, "rtsp_transport", "tcp", 0 );
	//av_dict_set( &ID.StreamOptions, "timeout", "600000", 0 );
	av_dict_set( &ID.StreamOptions, "buffer_size", "20971520", 0 );

	av_dict_set( &ID.StreamOptions, "nobuffer", "1", 0 );
	
	IsConnecting = true;
	TimeStarted = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	int Result = avformat_open_input( &ID.FormatContext, ID.Path.c_str(), nullptr, &ID.StreamOptions );
	IsConnecting = false;

	if( Result < 0 )
	{
		STREAM_ERROR( ConnectionError );
	}

	if( avformat_find_stream_info( ID.FormatContext, nullptr ) < 0 )
	{
		STREAM_ERROR( NoStreams );
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
		STREAM_ERROR( NoH264Support );
	}

	ID.CodecContext = avcodec_alloc_context3( nullptr );
	avcodec_get_context_defaults3( ID.CodecContext, OutputCodec );
	avcodec_parameters_to_context( ID.CodecContext, ID.FormatContext->streams[ ID.ChosenStreamIndex ]->codecpar );
	
	if( avcodec_open2( ID.CodecContext, OutputCodec, nullptr ) < 0 )
	{
		STREAM_ERROR( UnsupportedStreamFormat );
	}

	unsigned int OutputHeight = min( 400, ID.CodecContext->height );
	unsigned int OutputWidth = (int)(((float)ID.CodecContext->width / (float)ID.CodecContext->height) * (float)OutputHeight);

	OutputHeight &= (~15);
	OutputWidth &= (~15);

	AVPixelFormat OutputPixelFormat = AV_PIX_FMT_BGR24;

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

	return CameraStreamError::Success;
}

CameraStreamError InputStream::ProcessFrame( IRecordFilter* Filter, Stream* TargetStream )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;

	IsConnecting = true;
	TimeStarted = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	int Result = av_read_frame( m_InternalData->FormatContext, &ID.Packet );
	IsConnecting = false;
	if( Result == AVERROR_EOF )
	{
		return CameraStreamError::EndOfFile;
	}
	else if( Result < 0 )
	{
		STREAM_ERROR( FrameError );
	}

	OutputStream* Output = dynamic_cast<OutputStream*>(TargetStream);

	if( Output && !ID.PacketsSinceKeyframe.empty() )
	{
		for( auto& Packet : ID.PacketsSinceKeyframe )
		{
			CameraStreamError WriteError = Output->WriteInterleavedPacket( &ID.FormatContext->streams[ID.ChosenStreamIndex]->time_base, &Packet );
			if( WriteError != CameraStreamError::Success )
			{
				return WriteError;
			}
		}

		ID.FreeQueuedPackets();
	}

	if( ID.Packet.flags & AV_PKT_FLAG_KEY )
	{
		ID.FreeQueuedPackets();
	}

	//Add the new packet to the buffer, if we're not already producing output
	if (Output)
	{
		AVPacket NewPacket;
		av_copy_packet( &NewPacket, &ID.Packet );

		CameraStreamError WriteError = Output->WriteInterleavedPacket( &ID.FormatContext->streams[ID.ChosenStreamIndex]->time_base, &NewPacket );
		if( WriteError != CameraStreamError::Success )
		{
			return WriteError;
		}

		av_packet_unref( &NewPacket );
	}
	else
	{
		ID.PacketsSinceKeyframe.push_back( AVPacket() );
		AVPacket& NewPacket = ID.PacketsSinceKeyframe.back();
		av_copy_packet( &NewPacket, &ID.Packet );
	}

	if( ID.Packet.stream_index == ID.ChosenStreamIndex )
	{
		Result = avcodec_send_packet( m_InternalData->CodecContext, &ID.Packet );
		if( Result < 0 )
		{
			STREAM_ERROR( PacketError );
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
				STREAM_ERROR( DecoderReceiverError );
			}
			else
			{
				int OutputSliceSize = sws_scale( m_InternalData->ConversionContext, ID.Input->GetFrame()->data, ID.Input->GetFrame()->linesize, 0, m_InternalData->CodecContext->height, ID.Output->GetFrame()->data, ID.Output->GetFrame()->linesize );

				ClassificationResult FilterResult = Filter->FilterFrame( ID.Output->GetWidth(), ID.Output->GetHeight(), ID.Output->GetFrame()->data[0], m_StreamManager );

				if( FilterResult.ResultString )
				{
					ClassificationResult ResultNew = Filter->PostSuccessChildVisitor( ID.Output->GetWidth(), ID.Output->GetHeight(), ID.Output->GetFrame()->data[0], m_StreamManager );
					if( ResultNew.ResultString )
					{
						FilterResult = ResultNew;
					}
				}

				ID.Input->Unref();
			}
		}
	}

	av_packet_unref( &ID.Packet );
	av_init_packet( &ID.Packet );

	return CameraStreamError::Success;
}

void InputStream::Shutdown()
{
	auto& ID = *m_InternalData;

	if( m_StreamManager )
	{
		m_StreamManager->CloseDiagnosticStream();
	}

	if( ID.FormatContext )
	{
		avformat_close_input( &ID.FormatContext );
		avformat_free_context( ID.FormatContext );
		ID.FormatContext = nullptr;
	}

	Stream::Shutdown();
}

int InputStream::InterruptCallback( void* Opaque )
{
	InputStream* This = (InputStream*)Opaque;

	if( This->IsConnecting )
	{
		int64_t TimeNow = std::chrono::high_resolution_clock::now().time_since_epoch().count();

		if (TimeNow - (ConnectionTimeout * 1000ll * 1000ll * 1000ll) > This->TimeStarted)
		{
			return 1;
		}
	}

	return 0;
}

}}
