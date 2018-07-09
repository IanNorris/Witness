#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

namespace Witness{
namespace Camera{

int OutputStream::GlobalOutputStreamIndex = 0;

OutputStream::OutputStream( const std::string& Path, InputStream * InputStream )
: Stream()
, m_InputStream( InputStream )
, FrameIndex( 0 )
, StreamIndex( GlobalOutputStreamIndex++ )
{
	m_InputStream->Initialize();

	auto& ID = *m_InternalData;
	auto& InID = m_InputStream->GetData();

	if( InID.CodecContext )
	{
		const AVCodecContext& DecoderContext = *(InID.CodecContext);

		ID.PixelFormat = DecoderContext.pix_fmt;
		ID.CodecID = DecoderContext.codec_id;
		ID.CodecTag = DecoderContext.codec_tag;

		ID.Width = DecoderContext.width;
		ID.Height = DecoderContext.height;

		InputStream->GetFramerate( &ID.Framerate );
		InputStream->GetTimebase( &ID.Timebase );

		ID.AspectRatio = DecoderContext.sample_aspect_ratio;

		m_InternalData->Path = Path;
	}
	else
	{
		ID.Framerate.den = 0;
	}
}

OutputStream::OutputStream( const std::string& Path, unsigned int Width, unsigned int Height, int Framerate, bool IsBGR )
: Stream()
, m_InputStream( nullptr )
, FrameIndex( 0 )
{
	auto& ID = *m_InternalData;

	ID.Path = Path;

	ID.Width = Width;
	ID.Height= Height;
	ID.Framerate.num = Framerate;
	ID.Framerate.den = 1;
	ID.Timebase.num = 90000;
	ID.Timebase.den = 1;

	ID.PixelFormat = IsBGR ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24;
	ID.CodecID = AV_CODEC_ID_H264;
	ID.CodecTag = 0;

	ID.AspectRatio.num = 1;
	ID.AspectRatio.den = 1;

}

OutputStream::~OutputStream()
{}

CameraStreamError OutputStream::Initialize()
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

	//Constructor failed because input was invalid
	if ( ID.Framerate.den == 0 )
	{
		STREAM_ERROR( NoStreamInput, 0 );
	}

	av_init_packet( &ID.Packet );

	int Result = avformat_alloc_output_context2( &ID.FormatContext, nullptr, nullptr, ID.Path.c_str() );
	if( Result < 0 || !ID.FormatContext )
	{
		STREAM_ERROR( UnknownError, Result );
	}

	AVCodec* Encoder = avcodec_find_encoder( ID.CodecID );

	if( !Encoder )
	{
		STREAM_ERROR( NoH264Support, 0 );
	}

	AVStream* OutStream = avformat_new_stream( ID.FormatContext, Encoder );
	if( !OutStream )
	{
		STREAM_ERROR( UnknownError, 0 );
	}

	ID.CodecContext = avcodec_alloc_context3( Encoder );
	if( !ID.CodecContext )
	{
		STREAM_ERROR( NoH264Support, 0 );
	}

	if( ID.IsVideo )
	{
		ID.CodecContext->width = ID.Width;
		ID.CodecContext->height = ID.Height;
		ID.CodecContext->sample_aspect_ratio = ID.AspectRatio;
		ID.CodecContext->framerate = ID.Framerate;
		ID.CodecContext->time_base = av_inv_q(ID.Framerate);

		if( Encoder->pix_fmts )
		{
			ID.CodecContext->pix_fmt = Encoder->pix_fmts[0];
		}
		else
		{
			ID.CodecContext->pix_fmt = ID.PixelFormat;
		}
	}
	else
	{
		if( !m_InputStream )
		{
			STREAM_ERROR( UnsupportedStreamType, 0 );
		}
	}

	if( m_InputStream )
	{
		AVCodecParameters* Params = avcodec_parameters_alloc();
		avcodec_parameters_from_context( Params, m_InputStream->GetData().CodecContext );

		int OutputPixelFormat = Params->format;

		//Remap deprecated formats to avoid the warning output.
		switch(OutputPixelFormat)
		{
		case AV_PIX_FMT_YUVJ420P:
			OutputPixelFormat = AV_PIX_FMT_YUV420P;
			break;

		case AV_PIX_FMT_YUVJ422P:
			OutputPixelFormat = AV_PIX_FMT_YUV422P;
			break;

		case AV_PIX_FMT_YUVJ444P:
			OutputPixelFormat = AV_PIX_FMT_YUV444P;
			break;

		case AV_PIX_FMT_YUVJ440P:
			OutputPixelFormat = AV_PIX_FMT_YUV440P;
			break;
		}

		Params->format = OutputPixelFormat;
		Params->codec_tag = 0;
		Params->codec_id = ID.CodecID;
		avcodec_parameters_to_context( ID.CodecContext, Params );
		avcodec_parameters_free( &Params );
	}
	else
	{
		AVCodecParameters* CodecParams = OutStream->codecpar;

		CodecParams->width = ID.Width;
		CodecParams->height = ID.Height;
		CodecParams->format = AV_PIX_FMT_YUV420P;
		CodecParams->codec_id = ID.CodecID;
		CodecParams->codec_type = AVMEDIA_TYPE_VIDEO;
		CodecParams->profile = FF_PROFILE_H264_MAIN;
		CodecParams->level = 40;

		Result = avcodec_parameters_to_context( ID.CodecContext, CodecParams );
		if( Result < 0 )
		{
			STREAM_ERROR( EncoderCreationError, Result );
		}
	}

	AVDictionary* EncoderOptions = nullptr;
	/*av_dict_set( &EncoderOptions, "deadline", "realtime", 0 );
	av_dict_set( &EncoderOptions, "speed", "8", 0 );*/
	/*Result = av_dict_set( &EncoderOptions, "x264-params", "keyint", 1 );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError );
	}

	Result = av_dict_set( &EncoderOptions, "sdr_x264_preset", "fast", 0 );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError );
	}

	Result = av_dict_set( &EncoderOptions, "sdr_x264_crf", "0", 0 );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError );
	}*/

	Result = avcodec_open2( ID.CodecContext, Encoder, &EncoderOptions );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError, Result );
	}

	Result = avcodec_parameters_from_context( OutStream->codecpar, ID.CodecContext );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError, Result );
	}

	if( ID.FormatContext->oformat->flags & AVFMT_GLOBALHEADER )
	{
		ID.CodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	OutStream->time_base = ID.CodecContext->time_base;
	OutStream->avg_frame_rate= av_inv_q(OutStream->time_base);

	if( !( ID.FormatContext->oformat->flags & AVFMT_NOFILE ) )
	{
		Result = avio_open( &ID.FormatContext->pb, ID.Path.c_str(), AVIO_FLAG_WRITE );
		if( Result < 0 )
		{
			STREAM_ERROR( FileNotWriteable, Result );
		}
	}

	Result = avformat_write_header( ID.FormatContext, nullptr );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed, Result );
	}

	ID.Output = std::make_unique<FFMPEG::Frame>( ID.CodecContext->width, ID.CodecContext->height, ID.CodecContext->pix_fmt );
	//ID.Output->Prepare(); //Necessary?

	//Remap deprecated formats to avoid the warning output.
	switch(ID.PixelFormat)
	{
	case AV_PIX_FMT_YUVJ420P:
		ID.PixelFormat = AV_PIX_FMT_YUV420P;
		break;

	case AV_PIX_FMT_YUVJ422P:
		ID.PixelFormat = AV_PIX_FMT_YUV422P;
		break;

	case AV_PIX_FMT_YUVJ444P:
		ID.PixelFormat = AV_PIX_FMT_YUV444P;
		break;

	case AV_PIX_FMT_YUVJ440P:
		ID.PixelFormat = AV_PIX_FMT_YUV440P;
		break;
	}

	ID.ConversionContext = sws_getCachedContext(
		ID.ConversionContext,
		ID.Width,
		ID.Height,
		ID.PixelFormat,
		ID.CodecContext->width,
		ID.CodecContext->height,
		ID.CodecContext->pix_fmt,
		SWS_BICUBIC,
		NULL,
		NULL,
		NULL );

	return CameraStreamError::Success;
}

CameraStreamError OutputStream::ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;

	return CameraStreamError::Success;
}

CameraStreamError OutputStream::WriteInterleavedPacket( const AVPacket* Packet )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	AVPacket PacketCopy;
	memset( &PacketCopy, 0, sizeof(PacketCopy) );
	int Result = av_packet_ref( &PacketCopy, Packet );
	if( Result < 0 )
	{
		STREAM_ERROR( RefError, Result );
	}

	auto& ID = *m_InternalData;
	
	if (ID.IsFirstFrame)
	{
		ID.DTS = Packet->dts;
		ID.PTS = Packet->pts;
		PacketCopy.dts = 0;
		PacketCopy.pts = 0;

		ID.IsFirstFrame = false;
	}
	else
	{
		PacketCopy.dts -= ID.DTS;
		PacketCopy.pts -= ID.PTS;
	}
	
	if( m_InputStream )
	{
		AVRational TimeBase;
		m_InputStream->GetTimebase( &TimeBase );

		av_packet_rescale_ts( 
			&PacketCopy, 
			TimeBase,
			ID.FormatContext->streams[0]->time_base );
	}

	Result = av_interleaved_write_frame( ID.FormatContext, &PacketCopy );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed, Result );
	}

	FrameIndex++;

	return CameraStreamError::Success;
}

CameraStreamError OutputStream::WriteFrame( FFMPEG::Frame* Frame )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;

	ID.Output->Prepare();

	int OutputSliceSize = sws_scale( m_InternalData->ConversionContext, Frame->GetFrame()->data, Frame->GetFrame()->linesize, 0, Frame->GetHeight(), ID.Output->GetFrame()->data, ID.Output->GetFrame()->linesize );

	ID.Output->GetFrame()->pts = ID.CodecContext->frame_number;
	
	int Result = avcodec_send_frame( GetData().CodecContext, ID.Output->GetFrame() );
	if( Result == AVERROR(EAGAIN) )
	{
		CameraStreamError ResultErr = SendAll();
		if( ResultErr != CameraStreamError::Success )
		{
			return ResultErr;
		}
		Result = avcodec_send_frame( GetData().CodecContext, ID.Output->GetFrame() );
	}

	if( Result == 0 )
	{
		CameraStreamError ResultErr = SendAll();
		if( ResultErr != CameraStreamError::Success )
		{
			return ResultErr;
		}
	}

	FrameIndex++;
	
	return CameraStreamError::Success;
}

CameraStreamError OutputStream::SendAll( void )
{
	auto& ID = *m_InternalData;

	int Result;
	do 
	{
		AVPacket TempPacket = {};
		av_init_packet( &TempPacket );

		Result = avcodec_receive_packet( GetData().CodecContext, &TempPacket );
		if( Result == 0 )
		{
			av_packet_rescale_ts( &TempPacket, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base );

			TempPacket.stream_index = ID.FormatContext->streams[0]->index;

			Result = av_interleaved_write_frame( ID.FormatContext, &TempPacket );
			if( Result < 0 )
			{
				STREAM_ERROR( WriteFailed, Result );
			}

			av_packet_unref( &TempPacket );
		}
		else if( Result != AVERROR(EAGAIN) )
		{
			continue;
		}
		else if( Result != AVERROR_EOF )
		{
			break;
		}
		else if( Result < 0 )
		{
			STREAM_ERROR( WriteFailed, Result );
		}
	} while ( Result == 0);

	return CameraStreamError::Success;
}

void OutputStream::Shutdown()
{
	auto& ID = *m_InternalData;

	if( ID.FormatContext )
	{
		CloseFile();
				
		avformat_free_context( ID.FormatContext );
		ID.FormatContext = nullptr;
	}

	Stream::Shutdown();
}

CameraStreamError OutputStream::CloseFile()
{
	auto& ID = *m_InternalData;

	while( true )
	{
		//Flush
		int Result = avcodec_send_frame( ID.CodecContext, nullptr );
		if( Result == 0 )
		{
			CameraStreamError StrError = SendAll();
			if( StrError != CameraStreamError::Success )
			{
				return StrError;
			}
		}
		else if( Result == AVERROR_EOF )
		{
			break;
		}
		else
		{
			STREAM_ERROR( WriteFailed, Result );
		}
	}
	
	int Result = av_write_trailer( ID.FormatContext );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed, Result );
	}

	if( !(ID.FormatContext->oformat->flags& AVFMT_NOFILE) )
	{
		Result = avio_close( ID.FormatContext->pb );
		if( Result < 0 )
		{
			STREAM_ERROR( WriteFailed, Result );
		}
	}


	return CameraStreamError::Success;
}

int OutputStream::GetStreamIndex()
{
	return StreamIndex;
}

}}
