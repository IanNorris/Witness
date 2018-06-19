#include "InputStream.h"
#include "OutputStream.h"
#include "StreamManager.h"
#include "StreamData.h"
#include "ImageProcessingData.h"

#include <vector>
#include <iostream>
#include <chrono>

namespace Witness{
namespace Camera{

size_t MaxKeyFramesStored = 10; //This is the maximum buffer period in keyframes to maintain
size_t MinKeyFramesStored = 4; //This is the minimum buffer period in keyframes to maintain on a constant stream

InputStream::InputStream( int SourceID, ImageProcessingJobQueue* JobQueue, const std::string& StreamURL, int StreamIndex )
: Stream()
, CommonJobQueue( JobQueue )
, m_StreamManager( new StreamManager() )
, UniqueSourceID( SourceID )
, TimeStarted( 0 )
, IsConnecting( false )
{
	m_InternalData->Path = StreamURL;
	m_InternalData->StreamIndex = StreamIndex;
	m_InternalData->PacketsPerKeyframe.push_back(0);
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
	av_dict_set( &ID.StreamOptions, "recv_buffer_size", "8388608", 0 );

	av_dict_set( &ID.StreamOptions, "nobuffer", "0", 0 );
	
	IsConnecting = true;
	TimeStarted = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	int Result = avformat_open_input( &ID.FormatContext, ID.Path.c_str(), nullptr, &ID.StreamOptions );
	IsConnecting = false;

	if( Result < 0 )
	{
		STREAM_ERROR( ConnectionError, Result );
	}

	Result = avformat_find_stream_info( ID.FormatContext, nullptr );
	if( Result < 0 )
	{
		STREAM_ERROR( NoStreams, Result );
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
	
	AVFormatContext* OutputFormat = avformat_alloc_context();
	
	av_read_play( ID.FormatContext );

	AVCodec* OutputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if( !OutputCodec )
	{
		STREAM_ERROR( NoH264Support, 0 );
	}

	ID.CodecContext = avcodec_alloc_context3( nullptr );
	avcodec_get_context_defaults3( ID.CodecContext, OutputCodec );
	avcodec_parameters_to_context( ID.CodecContext, ID.FormatContext->streams[ ID.ChosenStreamIndex ]->codecpar );
	
	Result = avcodec_open2( ID.CodecContext, OutputCodec, nullptr );
	if( Result < 0 )
	{
		STREAM_ERROR( UnsupportedStreamFormat, Result );
	}

	/*unsigned int OutputHeight = min( 400, ID.CodecContext->height );
	unsigned int OutputWidth = (int)(((float)ID.CodecContext->width / (float)ID.CodecContext->height) * (float)OutputHeight);

	OutputHeight &= (~15);
	OutputWidth &= (~15);*/

	AVPixelFormat OutputPixelFormat = AV_PIX_FMT_BGR24;
		
	//ID.Output = std::make_shared<FFMPEG::Frame>( OutputWidth, OutputHeight, OutputPixelFormat );
	ID.Input = std::make_shared<FFMPEG::Frame>( ID.CodecContext->width, ID.CodecContext->height, ID.CodecContext->pix_fmt );

	/*ID.ConversionContext = sws_getCachedContext(
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
		NULL );*/

	return CameraStreamError::Success;
}

CameraStreamError InputStream::ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;

	av_init_packet( &ID.Packet );

	IsConnecting = true;
	TimeStarted = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	int Result = av_read_frame( m_InternalData->FormatContext, &ID.Packet );
	IsConnecting = false;
	if( Result == AVERROR_EOF )
	{
		STREAM_ERROR( EndOfFile, Result );
	}
	else if( Result < 0 )
	{
		STREAM_ERROR( FrameError, Result );
	}

	if( ID.Packet.stream_index == ID.ChosenStreamIndex )
	{
		OutputStream* Output = dynamic_cast<OutputStream*>(TargetStream);

		if( ID.Packet.flags & AV_PKT_FLAG_KEY )
		{
			ID.DeleteOldestKeyframe(MaxKeyFramesStored);
		}

		if( Output && !ID.PacketsBacklog.empty() )
		{
			for( auto& Packet : ID.PacketsBacklog )
			{
				CameraStreamError WriteError = Output->WriteInterleavedPacket( &ID.FormatContext->streams[ID.ChosenStreamIndex]->time_base, &Packet );
				if( WriteError != CameraStreamError::Success )
				{
					return WriteError;
				}
			}

			ID.FreeToMinimumBacklog(MinKeyFramesStored);
		}

		//Add the new packet to the buffer, if we're not already producing output
		if (Output)
		{
			AVPacket NewPacket;
			av_copy_packet( &NewPacket, &ID.Packet );

			CameraStreamError WriteError = Output->WriteInterleavedPacket( &ID.FormatContext->streams[ID.ChosenStreamIndex]->time_base, &NewPacket );
			if( WriteError != CameraStreamError::Success )
			{
				ID.FreeAllQueuedPackets();
				return WriteError;
			}

			av_packet_unref( &NewPacket );
		}
		else
		{
			ID.PacketsPerKeyframe.back()++;
			ID.PacketsBacklog.push_back( AVPacket() );
			AVPacket& NewPacket = ID.PacketsBacklog.back();
			av_copy_packet( &NewPacket, &ID.Packet );
		}

	
		Result = avcodec_send_packet( m_InternalData->CodecContext, &ID.Packet );
		if( Result == AVERROR(EAGAIN) )
		{
			return CameraStreamError::Success;
		}
		else if (Result == AVERROR_EOF)
		{
			return CameraStreamError::EndOfFile;
		}
		else if( Result < 0 )
		{
			STREAM_ERROR( PacketError, Result );
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
				STREAM_ERROR( DecoderReceiverError, Result );
			}
			else
			{
				auto Job = std::make_shared<ImageProcessingJob>();
				Job->Frame.swap(ID.Input);
				Job->SourceID = UniqueSourceID;
				Job->Timestamp = m_InternalData->DTS;
				Job->Filter = Filter;

				if (!CommonJobQueue->Push(Job))
				{
					auto Stats = CommonJobQueue->GetData().GetStatsForSource(UniqueSourceID);		

					double Total = (double)Stats.TotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
					double Scale = (double)Stats.ScaleTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
					double MD = (double)Stats.MotionDetectionTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
					double SP = (double)Stats.SecondPassFilterTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
					printf("Source %d: Total %.2fms, Scale: %.2fms, MD: %.2fms, 2p: %.2fms\n", UniqueSourceID, (float)Total, (float)Scale, (float)MD, (float)SP );

					CommonJobQueue->RemoveAllForSource(UniqueSourceID);

					STREAM_ERROR( ProcessingQueueFull, 0 );
				}
				
				ID.Input = std::make_shared<FFMPEG::Frame>( ID.CodecContext->width, ID.CodecContext->height, ID.CodecContext->pix_fmt );
			}
		}
	}

	av_packet_unref( &ID.Packet );

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
