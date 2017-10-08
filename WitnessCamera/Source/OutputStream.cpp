#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/samplefmt.h"

typedef struct FramePool {
    /**
     * Pools for each data plane. For audio all the planes have the same size,
     * so only pools[0] is used.
     */
    AVBufferPool *pools[4];

    /*
     * Pool parameters
     */
    int format;
    int width, height;
    int stride_align[AV_NUM_DATA_POINTERS];
    int linesize[4];
    int planes;
    int channels;
    int samples;
} FramePool;

typedef struct DecodeSimpleContext {
    AVPacket *in_pkt;
    AVFrame  *out_frame;
} DecodeSimpleContext;

typedef struct DecodeFilterContext {
    AVBSFContext **bsfs;
    int         nb_bsfs;
} DecodeFilterContext;

typedef struct AVCodecInternal {
    /**
     * Whether the parent AVCodecContext is a copy of the context which had
     * init() called on it.
     * This is used by multithreading - shared tables and picture pointers
     * should be freed from the original context only.
     */
    int is_copy;

    /**
     * Whether to allocate progress for frame threading.
     *
     * The codec must set it to 1 if it uses ff_thread_await/report_progress(),
     * then progress will be allocated in ff_thread_get_buffer(). The frames
     * then MUST be freed with ff_thread_release_buffer().
     *
     * If the codec does not need to call the progress functions (there are no
     * dependencies between the frames), it should leave this at 0. Then it can
     * decode straight to the user-provided frames (which the user will then
     * free with av_frame_unref()), there is no need to call
     * ff_thread_release_buffer().
     */
    int allocate_progress;

    /**
     * An audio frame with less than required samples has been submitted and
     * padded with silence. Reject all subsequent frames.
     */
    int last_audio_frame;

    AVFrame *to_free;

    FramePool *pool;

    void *thread_ctx;

    DecodeSimpleContext ds;
    DecodeFilterContext filter;

    /**
     * Properties (timestamps+side data) extracted from the last packet passed
     * for decoding.
     */
    AVPacket *last_pkt_props;

    /**
     * temporary buffer used for encoders to store their bitstream
     */
    uint8_t *byte_buffer;
    unsigned int byte_buffer_size;

    void *frame_thread_encoder;

    /**
     * Number of audio samples to skip at the start of the next decoded frame
     */
    int skip_samples;

    /**
     * hwaccel-specific private data
     */
    void *hwaccel_priv_data;

    /**
     * checks API usage: after codec draining, flush is required to resume operation
     */
    int draining;

    /**
     * buffers for using new encode/decode API through legacy API
     */
    AVPacket *buffer_pkt;
    int buffer_pkt_valid; // encoding: packet without data can be valid
    AVFrame *buffer_frame;
    int draining_done;
    /* set to 1 when the caller is using the old decoding API */
    int compat_decode;
    int compat_decode_warned;
    /* this variable is set by the decoder internals to signal to the old
     * API compat wrappers the amount of data consumed from the last packet */
    size_t compat_decode_consumed;
    /* when a partial packet has been consumed, this stores the remaining size
     * of the packet (that should be submitted in the next decode call */
    size_t compat_decode_partial_size;
    AVFrame *compat_decode_frame;

    int showed_multi_packet_warning;

    int skip_samples_multiplier;

    /* to prevent infinite loop on errors when draining */
    int nb_draining_errors;
} AVCodecInternal;

#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

namespace Witness{
namespace Camera{

OutputStream::OutputStream( const std::string& Path, InputStream * InputStream )
: Stream()
, m_InputStream( InputStream )
, FrameIndex( 0 )
{
	m_InputStream->Initialize();

	auto& ID = *m_InternalData;
	auto& InID = m_InputStream->GetData();
	const AVCodecContext& DecoderContext = *(InID.CodecContext);

	ID.PixelFormat = DecoderContext.pix_fmt;
	ID.CodecID = DecoderContext.codec_id;
	ID.CodecTag = DecoderContext.codec_tag;

	ID.Width = DecoderContext.width;
	ID.Height = DecoderContext.height;

	ID.Framerate.num = 25;
	ID.Framerate.den = 1;

	if( DecoderContext.framerate.num != 0 )
	{
		ID.Framerate = DecoderContext.framerate;
	}

	ID.AspectRatio = DecoderContext.sample_aspect_ratio;

	m_InternalData->Path = Path;
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

	av_init_packet( &ID.Packet );

	int Result = avformat_alloc_output_context2( &ID.FormatContext, nullptr, nullptr, ID.Path.c_str() );
	if( Result < 0 || !ID.FormatContext )
	{
		STREAM_ERROR( UnknownError );
	}

	AVCodec* Encoder = avcodec_find_encoder( ID.CodecID );

	if( !Encoder )
	{
		STREAM_ERROR( NoH264Support );
	}

	AVStream* OutStream = avformat_new_stream( ID.FormatContext, Encoder );
	if( !OutStream )
	{
		STREAM_ERROR( UnknownError );
	}

	ID.CodecContext = avcodec_alloc_context3( Encoder );
	if( !ID.CodecContext )
	{
		STREAM_ERROR( NoH264Support );
	}

	ID.CodecContext->time_base = av_inv_q(ID.Framerate);

	if( m_InputStream )
	{
		AVCodecParameters* Params = avcodec_parameters_alloc();
		avcodec_parameters_from_context( Params, m_InputStream->GetData().CodecContext );
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
		CodecParams->bit_rate = 441000;

		Result = avcodec_parameters_to_context( ID.CodecContext, CodecParams );
		if( Result < 0 )
		{
			STREAM_ERROR( EncoderCreationError );
		}
	}

	if( ID.IsVideo )
	{
		ID.CodecContext->width = ID.Width;
		ID.CodecContext->height = ID.Height;
		ID.CodecContext->sample_aspect_ratio = ID.AspectRatio;
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
			STREAM_ERROR( UnsupportedStreamType );
		}

		auto& InID = m_InputStream->GetData();
		const AVCodecContext& DecoderContext = *(InID.CodecContext);

		ID.CodecContext->sample_rate = DecoderContext.sample_rate;
		ID.CodecContext->channel_layout = DecoderContext.channel_layout;
		ID.CodecContext->channels = DecoderContext.channels;
		ID.CodecContext->sample_fmt = Encoder->sample_fmts[0];
		ID.CodecContext->time_base.num = 1;
		ID.CodecContext->time_base.den = ID.CodecContext->sample_rate;
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
	OutStream->avg_frame_rate= av_inv_q(OutStream->time_base);

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

	ID.Output = std::make_unique<FFMPEG::Frame>( ID.CodecContext->width, ID.CodecContext->height, ID.CodecContext->pix_fmt );

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

CameraStreamError OutputStream::ProcessFrame( IRecordFilter* Filter, Stream* TargetStream )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;

	return CameraStreamError::Success;
}

CameraStreamError OutputStream::WriteInterleavedPacket( AVRational* TimeBase, AVPacket* Packet )
{
	CameraStreamError InitError = Initialize();
	if( InitError != CameraStreamError::Success )
	{
		return InitError;
	}

	auto& ID = *m_InternalData;
	
	av_packet_rescale_ts( 
		Packet, 
		*TimeBase,
		ID.FormatContext->streams[0]->time_base );

	int Result = av_interleaved_write_frame( ID.FormatContext, Packet );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed );
	}

	FrameIndex++;

	return CameraStreamError::Success;
}

int avcodec_encode_video2_hax(AVCodecContext *avctx,
											  AVPacket *avpkt,
											  const AVFrame *frame,
											  int *got_packet_ptr)
{
	int ret;
	AVPacket user_pkt = *avpkt;
	int needs_realloc = !user_pkt.data;

	*got_packet_ptr = 0;

	if (!avctx->codec->encode2) {
		av_log(avctx, AV_LOG_ERROR, "This encoder requires using the avcodec_send_frame() API.\n");
		return AVERROR(ENOSYS);
	}


	if ((avctx->flags&AV_CODEC_FLAG_PASS1) && avctx->stats_out)
		avctx->stats_out[0] = '\0';

	if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY) && !frame) {
		av_packet_unref(avpkt);
		av_init_packet(avpkt);
		avpkt->size = 0;
		return 0;
	}

	if (av_image_check_size2(avctx->width, avctx->height, avctx->max_pixels, AV_PIX_FMT_NONE, 0, avctx))
		return AVERROR(EINVAL);

	if (frame && frame->format == AV_PIX_FMT_NONE)
		av_log(avctx, AV_LOG_WARNING, "AVFrame.format is not set\n");
	if (frame && (frame->width == 0 || frame->height == 0))
		av_log(avctx, AV_LOG_WARNING, "AVFrame.width or height is not set\n");
	
	ret = avctx->codec->encode2(avctx, avpkt, frame, got_packet_ptr);

	while(0);

	if (avpkt->data && avpkt->data == avctx->internal->byte_buffer) {
		needs_realloc = 0;
		if (user_pkt.data) {
			if (user_pkt.size >= avpkt->size) {
				memcpy(user_pkt.data, avpkt->data, avpkt->size);
			} else {
				av_log(avctx, AV_LOG_ERROR, "Provided packet is too small, needs to be %d\n", avpkt->size);
				avpkt->size = user_pkt.size;
				ret = -1;
			}
			avpkt->buf      = user_pkt.buf;
			avpkt->data     = user_pkt.data;
		} else if (!avpkt->buf) {
			AVPacket tmp = { 0 };
			ret = av_packet_ref(&tmp, avpkt);
			av_packet_unref(avpkt);
			if (ret < 0)
				return ret;
			*avpkt = tmp;
		}
	}

	if (!ret) {
		if (!*got_packet_ptr)
			avpkt->size = 0;
		else if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
			avpkt->pts = avpkt->dts = frame->pts;

		if (needs_realloc && avpkt->data) {
			ret = av_buffer_realloc(&avpkt->buf, avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
			if (ret >= 0)
				avpkt->data = avpkt->buf->data;
		}

		avctx->frame_number++;
	}

	if (ret < 0 || !*got_packet_ptr)
		av_packet_unref(avpkt);

	return ret;
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
	
	/*int Result = avcodec_send_frame( GetData().CodecContext, ID.Output->GetFrame() );
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
	}*/

	AVPacket enc_pkt;
	 enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);

	int got_packet = 0;
	int Result = avcodec_encode_video2_hax( ID.CodecContext, &enc_pkt, ID.Output->GetFrame(), &got_packet );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed );
	}

	av_packet_rescale_ts( &enc_pkt, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base );

			enc_pkt.stream_index = ID.FormatContext->streams[0]->index;

			Result = av_interleaved_write_frame( ID.FormatContext, &enc_pkt );
			if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed );
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
			/*if( TempPacket.pts != AV_NOPTS_VALUE )
			{
				TempPacket.pts = av_rescale_q_rnd( TempPacket.pts, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX) );
			}

			if( TempPacket.dts != AV_NOPTS_VALUE )
			{
				TempPacket.dts = av_rescale_q_rnd( TempPacket.dts, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX) );
			}

			if( TempPacket.duration != AV_NOPTS_VALUE )
			{
				TempPacket.duration = av_rescale_q( 1, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base );
			}*/

			av_packet_rescale_ts( &TempPacket, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base );

			TempPacket.stream_index = ID.FormatContext->streams[0]->index;

			avio_write( ID.FormatContext->pb, TempPacket.data, TempPacket.size );
			//Result = av_interleaved_write_frame( ID.FormatContext, &TempPacket );
			if( Result < 0 )
			{
				STREAM_ERROR( WriteFailed );
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
			STREAM_ERROR( WriteFailed );
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
			STREAM_ERROR( WriteFailed );
		}
	}
	
	int Result = av_write_trailer( ID.FormatContext );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed );
	}

	if( !(ID.FormatContext->oformat->flags& AVFMT_NOFILE) )
	{
		Result = avio_close( ID.FormatContext->pb );
		if( Result < 0 )
		{
			STREAM_ERROR( WriteFailed );
		}
	}


	return CameraStreamError::Success;
}

}}
