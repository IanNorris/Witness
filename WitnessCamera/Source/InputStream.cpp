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

InputStream::InputStream( const InputStreamSetup& Setup, int SourceID, ImageProcessingJobQueue* JobQueue, const std::string& StreamURL, int StreamIndex )
: Stream()
, StreamSetup( Setup )
, CommonJobQueue( JobQueue )
, m_StreamManager( new StreamManager() )
, UniqueSourceID( SourceID )
, FrameIndex(0)
, TimeStarted( 0 )
, IsConnecting( false )
{
	m_InternalData->Path = StreamURL;
	m_InternalData->StreamIndex = StreamIndex;
	m_InternalData->KeyframeStates.push_back(KeyframeInfo());
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

	if( !StreamSetup.Validate() )
	{
 		STREAM_ERROR( InvalidSetup, 0 );
	}

	m_InternalData->HasInitialized = true;

	FrameIndex = 0;

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

	const AVCodec* OutputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if( !OutputCodec )
	{
		STREAM_ERROR( NoH264Support, 0 );
	}

	ID.CodecContext = avcodec_alloc_context3(OutputCodec);
	avcodec_parameters_to_context( ID.CodecContext, ID.FormatContext->streams[ ID.ChosenStreamIndex ]->codecpar );
	
	//Export motion vectors for use by our motion detection algorithm
	AVDictionary* CodecOptions = nullptr;
	if( StreamSetup.ExportMotionVectors )
	{
		av_dict_set( &CodecOptions, "flags2", "+export_mvs", 0 );
	}

	Result = avcodec_open2( ID.CodecContext, OutputCodec, &CodecOptions );
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

	return CameraStreamError::Success;
}

CameraStreamError InputStream::ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream )
{
	auto ProcessingStart = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;

	IsConnecting = true;
	TimeStarted = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	
	auto ReadStart = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	int Result = av_read_frame( m_InternalData->FormatContext, &ID.Packet );

	auto ReadEnd = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	IsConnecting = false;
	if( Result == AVERROR_EOF )
	{
		STREAM_ERROR( EndOfFile, Result );
	}
	else if( Result < 0 )
	{
		STREAM_ERROR( FrameError, Result );
	}

	uint64_t CurrentTime = StreamSetup.GetTimestamp();

	uint64_t OutputStart = 0;
	uint64_t OutputEnd = 0;

	if( ID.Packet.stream_index == ID.ChosenStreamIndex )
	{
		OutputStart = std::chrono::high_resolution_clock::now().time_since_epoch().count();

		OutputStream* Output = dynamic_cast<OutputStream*>(TargetStream);

		if( ID.Packet.flags & AV_PKT_FLAG_KEY )
		{
			ID.DeleteOldestKeyframe( CurrentTime, StreamSetup.HistoricalPacketBufferSeconds );
		}

		if( Output && !ID.KeyframeStates.empty() )
		{
			int WrittenFrames = 0;

			size_t PacketBase = 0;
			for( auto& Keyframe : ID.KeyframeStates )
			{
				//Only process valid keyframes, and those we haven't touched already
				if( Keyframe.Timestamp > 0 && Keyframe.StreamIndex < Output->GetStreamIndex() )
				{
					Keyframe.StreamIndex = Output->GetStreamIndex();

					for( size_t PacketIndex = 0; PacketIndex < Keyframe.PacketCount; PacketIndex++ )
					{
						auto& Packet = ID.PacketsBacklog[PacketBase+PacketIndex];

						//printf( "BL DTS=%" PRId64 ", PTS=%" PRId64 ", Dur=%" PRId64 "\n", Packet.dts, Packet.pts, Packet.duration );

						WrittenFrames++;

						CameraStreamError WriteError = Output->WriteInterleavedPacket( &Packet );
						if( WriteError != CameraStreamError::Success )
						{
							return WriteError;
						}
					}
				}

				PacketBase += Keyframe.PacketCount;
			}
		}

		//Add the new packet to the buffer, if we're not already producing output
		if (Output)
		{
			//printf( "PT DTS=%" PRId64 ", PTS=%" PRId64 ", Dur=%" PRId64 "\n", ID.Packet.dts, ID.Packet.pts, ID.Packet.duration );

			CameraStreamError WriteError = Output->WriteInterleavedPacket( &ID.Packet );
			if( WriteError != CameraStreamError::Success )
			{
				ID.FreeAllQueuedPackets();
				return WriteError;
			}

			//av_packet_unref( &NewPacket );
		}

		//Add the new packet to the live stream
		if (LiveStream)
		{
			CameraStreamError WriteError = LiveStream->WriteInterleavedPacket(&ID.Packet);
			if (WriteError != CameraStreamError::Success)
			{
				memcpy(m_ErrorMessage, LiveStream->GetFFMPEGErrorMessage(), 256);
				ID.FreeAllQueuedPackets();
				return WriteError;
			}
		}
		

		{
			//Copy packet to the backlog
			auto& State = ID.KeyframeStates.back();
			State.PacketCount++;
			State.StreamIndex = Output ? Output->GetStreamIndex() : -1;
			if( State.Timestamp == 0)
			{
				State.Timestamp = CurrentTime;
			}
			ID.PacketsBacklog.push_back( AVPacket() );
			AVPacket& NewPacket = ID.PacketsBacklog.back();
			memset( &NewPacket, 0, sizeof(NewPacket) );
			Result = av_packet_ref( &NewPacket, &ID.Packet );
			if (Result < 0)
			{
				STREAM_ERROR( RefError, Result );
			}

			//No idea why, but the first packet always
			//has an invalid DTS/PTS that's higher than
			//the packets that follow it.
			if (FrameIndex == 0)
			{
				NewPacket.dts = 0;
				NewPacket.pts = 0;
			}
		}

		OutputEnd = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	
		Result = avcodec_send_packet( m_InternalData->CodecContext, &ID.Packet );
		if( Result == AVERROR(EAGAIN) )
		{
			av_packet_unref( &ID.Packet );
			return CameraStreamError::Success;
		}
		else if (Result == AVERROR_EOF)
		{
			av_packet_unref( &ID.Packet );
			return CameraStreamError::EndOfFile;
		}
		else if( Result < 0 )
		{
			av_packet_unref( &ID.Packet );
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
				av_packet_unref( &ID.Packet );
				STREAM_ERROR( DecoderReceiverError, Result );
			}
			else
			{
				if( (FrameIndex++ % StreamSetup.MotionFilterFrameSkip) == 0 )
				{
					auto Queue = CommonJobQueue;

					auto Job = std::make_shared<ClassificationTask>( std::make_shared<FilterFrameOwner>( ID.Input, Queue->GetData().GetStateForSource( UniqueSourceID )->FrameContext ));
					ID.Input.reset();

					Job->Frame.SourceID = UniqueSourceID;
					Job->Frame.TargetHeight = StreamSetup.MotionDetectFrameHeight;
					Job->Frame.Timestamp = CurrentTime;

					Job->Origin = Filter;
					Job->Next = Filter;
					
					
					Job->InsertToQueue = [Queue](SharedClassificationTask JobIn, bool HighPriority)
					{
						if( !Queue->Push(JobIn, HighPriority) )
						{
							auto Stats = Queue->GetData().GetStatsForSource(JobIn->Frame.SourceID);		

							/*double Total = (double)Stats.TotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
							double Scale = (double)Stats.ScaleTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
							double MD = (double)Stats.MotionDetectionTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
							double SP = Stats.SecondPassFrameCount ? (double)Stats.SecondPassFilterTotalProcessingTime / ((double)Stats.SecondPassFrameCount * 1000.0 * 1000.0) : 0.0;
							printf("Source %d: Total %.2fms, Scale: %.2fms, MD: %.2fms, 2p: %.2fms\n", UniqueSourceID, (float)Total, (float)Scale, (float)MD, (float)SP );
							*/

							printf("Backlog full for source %d\n", JobIn->Frame.SourceID);

							Queue->RemoveAllForSource(JobIn->Frame.SourceID);
							//CommonJobQueue->ResetStats(UniqueSourceID);
						}
					};

					if (!CommonJobQueue->Push(Job, false))
					{
						auto Stats = CommonJobQueue->GetData().GetStatsForSource(UniqueSourceID);		

						/*double Total = (double)Stats.TotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
						double Scale = (double)Stats.ScaleTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
						double MD = (double)Stats.MotionDetectionTotalProcessingTime / ((double)Stats.FrameCount * 1000.0 * 1000.0);
						double SP = Stats.SecondPassFrameCount ? (double)Stats.SecondPassFilterTotalProcessingTime / ((double)Stats.SecondPassFrameCount * 1000.0 * 1000.0) : 0.0;
						printf("Source %d: Total %.2fms, Scale: %.2fms, MD: %.2fms, 2p: %.2fms\n", UniqueSourceID, (float)Total, (float)Scale, (float)MD, (float)SP );
						*/

						printf("Backlog full for source %d\n", UniqueSourceID);

						CommonJobQueue->RemoveAllForSource(UniqueSourceID);
						//CommonJobQueue->ResetStats(UniqueSourceID);
					}
				
					ID.Input = std::make_shared<FFMPEG::Frame>( ID.CodecContext->width, ID.CodecContext->height, ID.CodecContext->pix_fmt );
				}
				else
				{
					ID.Input->Unref();
				}
			}
		}
	}

	av_packet_unref( &ID.Packet );

	auto ProcessingEnd = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	uint64_t ReadDiff = ReadEnd - ReadStart;
	uint64_t OutputDiff = OutputEnd - OutputStart;

	Stats.FrameCount++;
	Stats.DecoderTimeTotal += (ProcessingEnd - ProcessingStart) - ReadDiff - OutputDiff;
	Stats.OutputTimeTotal += OutputDiff;
	Stats.ReadTimeTotal += ReadDiff;

	//Sleep(10);

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

	if( ID.CodecContext )
	{
		avcodec_free_context( &ID.CodecContext );
		ID.CodecContext = nullptr;
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

bool InputStreamSetup::Validate()
{
	if(		GetTimestamp == nullptr
		||	MotionFilterFrameSkip <= 0
		||	MotionFilterFrameSkip > 50 
		||	MotionDetectFrameHeight < 20
		||	MotionDetectThreshold <= 0.0
		||	MotionDetectThreshold >= 1.0
	)
	{
 		return false;
	}

	return true;
}

void InputStream::GetTimebase( AVRational* TimebaseOut )
{
	auto& ID = *m_InternalData;
	*TimebaseOut = ID.FormatContext->streams[ID.ChosenStreamIndex]->time_base;
}

void InputStream::GetFramerate( AVRational* FramerateOut )
{
	auto& ID = *m_InternalData;
	*FramerateOut = ID.CodecContext->framerate;
}

double InputStream::GetFramerateDouble()
{
	auto& ID = *m_InternalData;

	if (ID.CodecContext && ID.CodecContext->framerate.num > 0 && ID.CodecContext->framerate.den > 0 )
	{
		return (double)ID.CodecContext->framerate.num / (double)ID.CodecContext->framerate.den;
	}
	else
	{
		return 25.0;
	}
}

CameraStreamError InputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	STREAM_ERROR(InvalidSetup, 0);

	//Not implemented
	assert(false);
}

}}
