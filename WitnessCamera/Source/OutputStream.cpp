#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

namespace Witness{
namespace Camera{

OutputStream::OutputStream( const std::string& Path, InputStream * InputStream )
: Stream()
, m_InputStream( InputStream )
{
	m_InternalData->Path = Path;
}

OutputStream::~OutputStream()
{}

CameraStreamError OutputStream::Initialize()
{
	CameraStreamError StreamInitResult = Stream::Initialize();
	if( StreamInitResult != CameraStreamError::Success )
	{
		return StreamInitResult;
	}

	auto& ID = *m_InternalData;
	auto& InID = m_InputStream->GetData();
	const AVCodecContext& DecoderContext = *(InID.CodecContext);

	avformat_alloc_output_context2( &ID.FormatContext, nullptr, nullptr, ID.Path.c_str() );
	if( !ID.FormatContext )
	{
		STREAM_ERROR( UnknownError );
	}

	AVStream* OutStream = avformat_new_stream( ID.FormatContext, nullptr );
	if( !OutStream )
	{
		STREAM_ERROR( UnknownError );
	}

	AVCodec* Encoder = avcodec_find_encoder( DecoderContext.codec_id );

	if( !Encoder )
	{
		STREAM_ERROR( NoH264Support );
	}

	ID.CodecContext = avcodec_alloc_context3( Encoder );
	if( !ID.CodecContext )
	{
		STREAM_ERROR( NoH264Support );
	}

	if( DecoderContext.codec_type== AVMEDIA_TYPE_VIDEO )
	{
		ID.CodecContext->width = DecoderContext.width;
		ID.CodecContext->height = DecoderContext.height;
		ID.CodecContext->sample_aspect_ratio = DecoderContext.sample_aspect_ratio;

		if( DecoderContext.framerate.num == 0 )
		{
			AVRational Framerate { 25, 1 };
			ID.CodecContext->time_base = av_inv_q(Framerate);
		}
		else 
		{
			ID.CodecContext->time_base = av_inv_q(DecoderContext.framerate);
		}

		if( Encoder->pix_fmts )
		{
			ID.CodecContext->pix_fmt = Encoder->pix_fmts[0];
		}
		else
		{
			ID.CodecContext->pix_fmt = DecoderContext.pix_fmt;
		}
	}
	else if( DecoderContext.codec_type == AVMEDIA_TYPE_AUDIO )
	{
		ID.CodecContext->sample_rate = DecoderContext.sample_rate;
		ID.CodecContext->channel_layout = DecoderContext.channel_layout;
		ID.CodecContext->channels = DecoderContext.channels;
		ID.CodecContext->sample_fmt = Encoder->sample_fmts[0];
		ID.CodecContext->time_base.num = 1;
		ID.CodecContext->time_base.den = ID.CodecContext->sample_rate;
	}
	else
	{
		STREAM_ERROR( UnsupportedStreamType );
	}

	int Result = avcodec_open2( ID.CodecContext, Encoder, nullptr );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError );
	}

	Result = avcodec_parameters_from_context( OutStream->codecpar, ID.CodecContext );
	if( Result < 0 )
	{
		STREAM_ERROR( EncoderCreationError );
	}

	if( ID.FormatContext->oformat->flags & AVFMT_GLOBALHEADER )
	{
		ID.CodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	OutStream->time_base = ID.CodecContext->time_base;

	if( !( ID.FormatContext->oformat->flags & AVFMT_NOFILE ) )
	{
		Result = avio_open( &ID.FormatContext->pb, ID.Path.c_str(), AVIO_FLAG_WRITE );
		if( Result < 0 )
		{
			STREAM_ERROR( FileNotWriteable );
		}
	}

	Result = avformat_write_header( ID.FormatContext, nullptr );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed );
	}

	return CameraStreamError::Success;
}

CameraStreamError OutputStream::ProcessFrame( IRecordFilter* Filter, Stream* TargetStream )
{
	auto& ID = *m_InternalData;

	return CameraStreamError::Success;
}

void OutputStream::Shutdown()
{
	auto& ID = *m_InternalData;

	if( ID.FormatContext )
	{
		if( !(ID.FormatContext->oformat->flags& AVFMT_NOFILE) )
		{
			avio_closep( &ID.FormatContext->pb );
		}

		avformat_free_context( ID.FormatContext );
		ID.FormatContext = nullptr;
	}

	Stream::Shutdown();
}

void OutputStream::CloseFile()
{
	auto& ID = *m_InternalData;

	if( ID.CodecContext->codec->capabilities & AV_CODEC_CAP_DELAY )
	{
		//TODO: Flush frames here?
	}

	av_write_trailer( ID.FormatContext );
}

}}
