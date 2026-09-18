#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "InMemoryIOContext.h"

#include <Log.h>
#include <algorithm>
#include <climits>

namespace Witness{
namespace Camera{

int OutputStream::GlobalOutputStreamIndex = 0;

OutputStream::OutputStream( const std::string& Path, InputStream * InputStream, bool InMemory, bool LiveStream, bool Part, bool InitSegment)
: Stream()
, m_InputStream( InputStream )
, m_IOContext( nullptr )
, FrameIndex( 0 )
, StreamIndex( GlobalOutputStreamIndex++ )
, m_FileOpened( false )
, m_InMemory( InMemory )
, m_Live(LiveStream)
, m_Part(Part)
, m_Isolated(false)
, m_InitSegment(InitSegment)
, m_Passthrough(false)
, m_ClipLength( 0.0 )
, m_SegmentIndex(-1)
, m_PartIndex(-1)
, m_LastWrittenDTS( AV_NOPTS_VALUE )
, m_LastWrittenAudioDTS( AV_NOPTS_VALUE )
, m_HasAudioStream( false )
, m_AudioInputStreamIndex( -1 )
, m_InitialTimestampUs( AV_NOPTS_VALUE )
, m_AudioTimestampOffsetEstablished( false )
, m_AudioTimestampOffsetUs( 0 )
, m_TimestampNormalizationAllowed( false )
, m_LastRawVideoDTS( AV_NOPTS_VALUE )
, m_LastNormalizedVideoDuration( 0 )
, m_TimestampCorrectionRemainder( 0 )
, m_RepairedVideoTimestamps( 0 )
, m_TimestampNormalizationActive( false )
, m_TimestampNormalizationRejected( false )
, m_TimestampProbeSamples( 0 )
, m_TimestampProbeOutliers( 0 )
, m_TimestampProbeDeltaTicks( 0 )
, m_TimestampProbeNominalTicks( 0 )
, m_QualifiedVideoDuration( 0 )
, m_TimestampSourceOffset( 0 )
{
	m_InputStream->Initialize();

	if (m_InMemory)
	{
		m_IOContext = new FFMPEG::InMemoryIOContext(Path.c_str());
	}

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
, m_IOContext( nullptr )
, FrameIndex( 0 )
, m_FileOpened( false )
, m_InMemory( false )
, m_Live( false )
, m_Part( false )
, m_Isolated(false)
, m_InitSegment(false)
, m_Passthrough(false)
, m_ClipLength( 0.0 )
, m_SegmentIndex(-1)
, m_LastWrittenDTS( AV_NOPTS_VALUE )
, m_LastWrittenAudioDTS( AV_NOPTS_VALUE )
, m_HasAudioStream( false )
, m_AudioInputStreamIndex( -1 )
, m_InitialTimestampUs( AV_NOPTS_VALUE )
, m_AudioTimestampOffsetEstablished( false )
, m_AudioTimestampOffsetUs( 0 )
, m_TimestampNormalizationAllowed( false )
, m_LastRawVideoDTS( AV_NOPTS_VALUE )
, m_LastNormalizedVideoDuration( 0 )
, m_TimestampCorrectionRemainder( 0 )
, m_RepairedVideoTimestamps( 0 )
, m_TimestampNormalizationActive( false )
, m_TimestampNormalizationRejected( false )
, m_TimestampProbeSamples( 0 )
, m_TimestampProbeOutliers( 0 )
, m_TimestampProbeDeltaTicks( 0 )
, m_TimestampProbeNominalTicks( 0 )
, m_QualifiedVideoDuration( 0 )
, m_TimestampSourceOffset( 0 )
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
{
	if( m_IOContext )
	{
		delete m_IOContext;
	}
}

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

	int Result = avformat_alloc_output_context2( &ID.FormatContext, nullptr, m_InMemory ? "mp4" : nullptr, m_InMemory ? nullptr : ID.Path.c_str());
	if( Result < 0 || !ID.FormatContext )
	{
		STREAM_ERROR( UnknownError, Result );
	}

	if (m_InMemory)
	{
		ID.FormatContext->pb = m_IOContext->GetContext();
	}

	// Passthrough mode: when we have an InputStream and won't be re-encoding frames,
	// copy codec parameters directly without creating an encoder. This allows any codec
	// (H.264, HEVC, etc.) to pass through even without the corresponding encoder library.
	if( m_InputStream && !m_Live )
	{
		m_Passthrough = true;

		auto& InID = m_InputStream->GetData();

		AVStream* OutStream = avformat_new_stream( ID.FormatContext, nullptr );
		if( !OutStream )
		{
			STREAM_ERROR( UnknownError, 0 );
		}

		AVStream* InStream = InID.FormatContext->streams[InID.ChosenStreamIndex];
		Result = avcodec_parameters_copy( OutStream->codecpar, InStream->codecpar );
		if( Result < 0 )
		{
			STREAM_ERROR( DecoderReceiverError, Result );
		}

		// Remap deprecated pixel formats
		switch( OutStream->codecpar->format )
		{
		case AV_PIX_FMT_YUVJ420P: OutStream->codecpar->format = AV_PIX_FMT_YUV420P; break;
		case AV_PIX_FMT_YUVJ422P: OutStream->codecpar->format = AV_PIX_FMT_YUV422P; break;
		case AV_PIX_FMT_YUVJ444P: OutStream->codecpar->format = AV_PIX_FMT_YUV444P; break;
		case AV_PIX_FMT_YUVJ440P: OutStream->codecpar->format = AV_PIX_FMT_YUV440P; break;
		}

		OutStream->codecpar->codec_tag = 0;
		OutStream->time_base = InStream->time_base;

		if( InID.HasAudio )
		{
			AVStream* AudioInStream = InID.FormatContext->streams[InID.ChosenAudioStreamIndex];
			AVStream* AudioOutStream = avformat_new_stream( ID.FormatContext, nullptr );
			if( !AudioOutStream || avcodec_parameters_copy( AudioOutStream->codecpar, AudioInStream->codecpar ) < 0 )
			{
				STREAM_ERROR( DecoderReceiverError, 0 );
			}
			AudioOutStream->codecpar->codec_tag = 0;
			AudioOutStream->time_base = AudioInStream->time_base;
			m_HasAudioStream = true;
			m_AudioInputStreamIndex = InID.ChosenAudioStreamIndex;
		}

		// Open output file
		if( !( ID.FormatContext->oformat->flags & AVFMT_NOFILE ) )
		{
			Result = avio_open( &ID.FormatContext->pb, ID.Path.c_str(), AVIO_FLAG_WRITE );
			if( Result < 0 )
			{
				STREAM_ERROR( FileNotWriteable, Result );
			}
			m_FileOpened = true;
		}

		Result = avformat_write_header( ID.FormatContext, nullptr );
		if( Result < 0 )
		{
			STREAM_ERROR( WriteFailed, Result );
		}

		return CameraStreamError::Success;
	}

	// Re-encoding path: create encoder for the target codec
	const AVCodec* Encoder = avcodec_find_encoder( ID.CodecID );

	if( !Encoder )
	{
		STREAM_ERROR( NoCodecSupport, 0 );
	}

	AVStream* OutStream = avformat_new_stream( ID.FormatContext, Encoder );
	if( !OutStream )
	{
		STREAM_ERROR( UnknownError, 0 );
	}

	ID.CodecContext = avcodec_alloc_context3( Encoder );
	if( !ID.CodecContext )
	{
		STREAM_ERROR( NoCodecSupport, 0 );
	}

	if( ID.IsVideo )
	{
		ID.CodecContext->width = ID.Width;
		ID.CodecContext->height = ID.Height;
		ID.CodecContext->sample_aspect_ratio = ID.AspectRatio;
		ID.CodecContext->framerate = ID.Framerate;
		ID.CodecContext->time_base = av_inv_q(ID.Framerate);

		ID.CodecContext->pix_fmt = ID.PixelFormat;
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
		Result = avcodec_parameters_from_context( Params, m_InputStream->GetData().CodecContext );
		if( Result < 0 )
		{
			STREAM_ERROR( DecoderReceiverError, Result );
		}

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

		OutStream->time_base = m_InputStream->GetData().FormatContext->streams[m_InputStream->GetData().ChosenStreamIndex]->time_base;
		ID.CodecContext->time_base = OutStream->time_base;

		Params->format = OutputPixelFormat;
		Params->codec_tag = 0;
		Params->codec_id = ID.CodecID;
		Result = avcodec_parameters_to_context( ID.CodecContext, Params );
		if( Result < 0 )
		{
			STREAM_ERROR( DecoderReceiverError, Result );
		}
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
		CodecParams->profile = AV_PROFILE_H264_MAIN;
		CodecParams->level = 40;

		Result = avcodec_parameters_to_context( ID.CodecContext, CodecParams );
		if( Result < 0 )
		{
			STREAM_ERROR( EncoderCreationError, Result );
		}
	}

	AVDictionary* EncoderOptions = nullptr;

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

	ID.CodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	OutStream->time_base = ID.CodecContext->time_base;
	OutStream->avg_frame_rate= av_inv_q(OutStream->time_base);

	if( !( ID.FormatContext->oformat->flags & AVFMT_NOFILE ) )
	{
		Result = avio_open( &ID.FormatContext->pb, ID.Path.c_str(), AVIO_FLAG_WRITE );
		if( Result < 0 )
		{
			STREAM_ERROR( FileNotWriteable, Result );
		}
		m_FileOpened = true;
	}

	//https://github.com/iinfer/leandromoreira_ffmpeg-libav-tutorial
	AVDictionary* options = nullptr;
	if (m_InitSegment)
	{
		av_dict_set(&options, "movflags", "empty_moov+default_base_moof+omit_tfhd_offset+dash", 0);
	}
	else if (m_Live && m_Part)
	{
		av_dict_set(&options, "movflags", "empty_moov+frag_keyframe+omit_tfhd_offset+dash", 0);
	}
	else if (m_Live)
	{
		av_dict_set(&options, "movflags", "empty_moov+omit_tfhd_offset+dash", 0);
	}

	Result = avformat_write_header( ID.FormatContext, &options);
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed, Result );
	}

	if (!m_Live)
	{
		ID.Output = std::make_unique<FFMPEG::Frame>(ID.CodecContext->width, ID.CodecContext->height, ID.CodecContext->pix_fmt);
	}

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

	if (!m_Live)
	{
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
			NULL);
	}

	return CameraStreamError::Success;
}

CameraStreamError OutputStream::ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* TargetStream2 )
{
	assert(m_Live == false);

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
	const bool IsAudio = m_HasAudioStream && PacketCopy.stream_index == m_AudioInputStreamIndex;
	if( !IsAudio && PacketCopy.stream_index != m_InputStream->GetData().ChosenStreamIndex )
	{
		av_packet_unref( &PacketCopy );
		return CameraStreamError::Success;
	}
	if( PacketCopy.dts == AV_NOPTS_VALUE )
	{
		// AAC from some RTSP cameras carries only PTS. Preserve it when possible;
		// otherwise discard this packet without forcing the camera to reconnect.
		if( !IsAudio || PacketCopy.pts == AV_NOPTS_VALUE )
		{
			av_packet_unref( &PacketCopy );
			return CameraStreamError::Success;
		}
		PacketCopy.dts = PacketCopy.pts;
	}
	if( PacketCopy.pts == AV_NOPTS_VALUE )
		PacketCopy.pts = PacketCopy.dts;

	auto& ID = *m_InternalData;
	
	if (m_Live)
	{
		
	}
	else
	{
		AVRational InputTimebase = m_InputStream->GetData().FormatContext->streams[Packet->stream_index]->time_base;
		int64_t PacketTimestampUs = av_rescale_q( PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q );
		if( m_InitialTimestampUs == AV_NOPTS_VALUE && !IsAudio )
			m_InitialTimestampUs = PacketTimestampUs;
		if( m_InitialTimestampUs == AV_NOPTS_VALUE || PacketTimestampUs < m_InitialTimestampUs )
		{
			av_packet_unref( &PacketCopy );
			return CameraStreamError::Success;
		}
		if( IsAudio && !m_AudioTimestampOffsetEstablished )
		{
			const int64_t AudioStartOffsetUs = PacketTimestampUs - m_InitialTimestampUs;
			if( AudioStartOffsetUs > 30 * AV_TIME_BASE )
			{
				m_AudioTimestampOffsetUs = AudioStartOffsetUs;
				LOG_WARNING( "Recording rebased audio timestamp origin by %.3fs", AudioStartOffsetUs / 1000000.0 );
			}
			m_AudioTimestampOffsetEstablished = true;
		}
		const int64_t StreamOriginUs = m_InitialTimestampUs + ( IsAudio ? m_AudioTimestampOffsetUs : 0 );
		if( PacketTimestampUs < StreamOriginUs )
		{
			av_packet_unref( &PacketCopy );
			return CameraStreamError::Success;
		}
		int64_t OffsetUs = PacketTimestampUs - StreamOriginUs;
		PacketCopy.dts = av_rescale_q( OffsetUs, AV_TIME_BASE_Q, InputTimebase );
		PacketCopy.pts = av_rescale_q( av_rescale_q(PacketCopy.pts, InputTimebase, AV_TIME_BASE_Q) - StreamOriginUs, AV_TIME_BASE_Q, InputTimebase );
		if( !IsAudio && ID.IsFirstFrame )
		{
			ID.DTS = 0;
			// PTS and DTS must share the same origin. Normalizing them
			// independently destroys the composition offset used by B-frames.
			ID.PTS = ID.DTS;
			ID.IsFirstFrame = false;
		}
	}

	PacketCopy.pos = -1;
	PacketCopy.stream_index = IsAudio ? 1 : 0;
	if( PacketCopy.duration < 0 || PacketCopy.duration > INT_MAX )
		PacketCopy.duration = 0;
	const bool TimestampRepaired = !IsAudio &&
		NormalizeVideoTimestamp(&PacketCopy, m_InputStream->GetData().FormatContext->
			streams[Packet->stream_index]->time_base);

	// Retain the conservative guard for generic/B-frame streams. Qualified
	// no-B-frame cameras have already been placed on a continuous output clock.
	const int64_t LastStreamDTS = IsAudio ? m_LastWrittenAudioDTS : m_LastWrittenDTS;
	if (LastStreamDTS != AV_NOPTS_VALUE && PacketCopy.dts <= LastStreamDTS)
	{
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	if (!IsAudio)
		m_LastWrittenDTS = PacketCopy.dts;
	else
		m_LastWrittenAudioDTS = PacketCopy.dts;

	// Clamp negative durations from B-frame reordering
	if (PacketCopy.duration < 0)
		PacketCopy.duration = 0;

	//Calc length before we adjust for the time base, otherwise we need to
	//adjust the calculation to the new timebase.
	if (m_Live)
	{
		m_ClipLength += (double)((PacketCopy.duration) * ID.FormatContext->streams[0]->time_base.num) / ID.FormatContext->streams[0]->time_base.den;
	}
	else
	{
		if (!IsAudio)
			m_ClipLength = (double)((PacketCopy.dts + PacketCopy.duration) * ID.Timebase.num) / ID.Timebase.den;
	}

	if( !m_Live )
	{
		av_packet_rescale_ts(
			&PacketCopy,
			m_InputStream->GetData().FormatContext->streams[Packet->stream_index]->time_base,
			ID.FormatContext->streams[PacketCopy.stream_index]->time_base);
	}

	Result = av_interleaved_write_frame( ID.FormatContext, &PacketCopy );
	if( Result < 0 )
	{
		STREAM_ERROR( WriteFailed, Result );
	}

	FrameIndex++;
	if( TimestampRepaired )
	{
		++m_RepairedVideoTimestamps;
		if( m_RepairedVideoTimestamps <= 5 || (m_RepairedVideoTimestamps % 500) == 0 )
			LOG_WARNING("Recording repaired jittery video timestamps (%llu packets)",
				(unsigned long long)m_RepairedVideoTimestamps);
	}

	return CameraStreamError::Success;
}

bool OutputStream::NormalizeVideoTimestamp(AVPacket* Packet, AVRational InputTimebase)
{
	if( !m_TimestampNormalizationAllowed || !m_InputStream || !Packet ||
		m_InputStream->GetData().CodecContext->has_b_frames != 0 )
		return false;

	const int64_t RawDTS = Packet->dts;
	const AVRational Framerate = m_InputStream->GetData().CodecContext->framerate;
	int64_t NominalDuration = 0;
	if( Framerate.num > 0 && Framerate.den > 0 )
	{
		const double FramesPerSecond = av_q2d(Framerate);
		if( FramesPerSecond >= 1.0 && FramesPerSecond <= 120.0 )
			NominalDuration = av_rescale_q(1, av_inv_q(Framerate), InputTimebase);
	}
	if( NominalDuration <= 0 && Packet->duration > 0 && Packet->duration <= INT_MAX )
		NominalDuration = Packet->duration;
	if( NominalDuration > INT_MAX )
		NominalDuration = 0;

	const int64_t ProbeDuration = m_TimestampNormalizationActive ?
		m_QualifiedVideoDuration : NominalDuration;
	if( m_LastRawVideoDTS != AV_NOPTS_VALUE && ProbeDuration > 0 &&
		!m_TimestampNormalizationRejected )
	{
		const int64_t Delta = RawDTS - m_LastRawVideoDTS;
		const int64_t Error = Delta - ProbeDuration;
		++m_TimestampProbeSamples;
		m_TimestampProbeDeltaTicks += Delta;
		m_TimestampProbeNominalTicks += ProbeDuration;
		if( std::abs(Error) * 2 > ProbeDuration )
			++m_TimestampProbeOutliers;
		if( !m_TimestampNormalizationActive && m_TimestampProbeSamples >= 40 )
		{
			const int64_t TotalError = m_TimestampProbeDeltaTicks - m_TimestampProbeNominalTicks;
			const bool AverageCadenceMatches = m_TimestampProbeDeltaTicks > 0 &&
				std::abs(TotalError) * 100 <= m_TimestampProbeNominalTicks * 5;
			const bool SourceClockIsJittery =
				m_TimestampProbeOutliers * 5 >= m_TimestampProbeSamples;
			if( AverageCadenceMatches && SourceClockIsJittery )
			{
				m_TimestampNormalizationActive = true;
				m_QualifiedVideoDuration = (std::max<int64_t>)(1,
					m_TimestampProbeNominalTicks / m_TimestampProbeSamples);
				LOG_INFO("Recording enabled guarded timestamp normalization after %d samples",
					m_TimestampProbeSamples);
			}
			else if( m_TimestampProbeSamples >= 120 )
			{
				m_TimestampProbeSamples = 0;
				m_TimestampProbeOutliers = 0;
				m_TimestampProbeDeltaTicks = 0;
				m_TimestampProbeNominalTicks = 0;
			}
		}
		if( m_TimestampNormalizationActive &&
			av_rescale_q(m_TimestampProbeNominalTicks, InputTimebase, AV_TIME_BASE_Q) >=
				120 * AV_TIME_BASE )
		{
			const int64_t TotalError = m_TimestampProbeDeltaTicks - m_TimestampProbeNominalTicks;
			if( std::abs(TotalError) * 100 > m_TimestampProbeNominalTicks * 5 )
			{
				m_TimestampNormalizationActive = false;
				m_TimestampNormalizationRejected = true;
				m_TimestampSourceOffset =
					(m_LastWrittenDTS + m_LastNormalizedVideoDuration) - RawDTS;
				LOG_WARNING("Recording disabled timestamp normalization after sustained cadence mismatch");
			}
			m_TimestampProbeSamples = 0;
			m_TimestampProbeOutliers = 0;
			m_TimestampProbeDeltaTicks = 0;
			m_TimestampProbeNominalTicks = 0;
		}
	}

	if( m_LastWrittenDTS == AV_NOPTS_VALUE )
	{
		Packet->pts = Packet->dts;
		if( Packet->duration <= 0 && NominalDuration > 0 )
			Packet->duration = NominalDuration;
		m_LastRawVideoDTS = RawDTS;
		m_LastNormalizedVideoDuration = Packet->duration > 0 ? Packet->duration : 1;
		return false;
	}

	const int64_t StepDuration = m_TimestampNormalizationActive ?
		m_QualifiedVideoDuration : (NominalDuration > 0 ? NominalDuration : m_LastNormalizedVideoDuration);
	const int64_t ExpectedDTS = m_LastWrittenDTS + (std::max<int64_t>)(1, m_LastNormalizedVideoDuration);
	if( !m_TimestampNormalizationActive )
	{
		const int64_t AdjustedRawDTS = RawDTS + m_TimestampSourceOffset;
		const bool Repaired = AdjustedRawDTS <= m_LastWrittenDTS;
		if( Repaired )
			Packet->dts = ExpectedDTS;
		else
			Packet->dts = AdjustedRawDTS;
		Packet->pts = Packet->dts;
		if( Packet->duration <= 0 && StepDuration > 0 )
			Packet->duration = StepDuration;
		m_LastRawVideoDTS = RawDTS;
		m_LastNormalizedVideoDuration = Packet->duration > 0 ? Packet->duration : 1;
		return Repaired;
	}

	const int64_t StableDuration = (std::min<int64_t>)(INT_MAX,
		(std::max<int64_t>)(1, StepDuration));
	const int64_t PhaseError = RawDTS - ExpectedDTS;
	const int64_t ResponseWindow = av_rescale_q(10 * AV_TIME_BASE, AV_TIME_BASE_Q, InputTimebase);
	const int64_t MaxCorrection = (std::min)(
		StableDuration / 20, (int64_t)INT_MAX - StableDuration);
	int64_t Correction = 0;
	if( ResponseWindow > 0 )
	{
		const int64_t PhaseLimit = (std::max<int64_t>)(1, ResponseWindow / 10);
		const int64_t BoundedError = (std::max)(-PhaseLimit, (std::min)(PhaseError, PhaseLimit));
		const int64_t Numerator = BoundedError * StableDuration + m_TimestampCorrectionRemainder;
		Correction = Numerator / ResponseWindow;
		m_TimestampCorrectionRemainder = Numerator % ResponseWindow;
	}
	Correction = (std::max)(-MaxCorrection, (std::min)(Correction, MaxCorrection));
	Packet->dts = ExpectedDTS;
	Packet->pts = ExpectedDTS;
	Packet->duration = (std::max<int64_t>)(1, StableDuration + Correction);
	m_LastRawVideoDTS = RawDTS;
	m_LastNormalizedVideoDuration = Packet->duration;
	return RawDTS != ExpectedDTS;
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

	ID.Output->GetFrame()->pts = FrameIndex;
	
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

		Result = avcodec_receive_packet( GetData().CodecContext, &TempPacket );
		if( Result == 0 )
		{
			if(!m_Live)
			{
				av_packet_rescale_ts(&TempPacket, ID.CodecContext->time_base, ID.FormatContext->streams[0]->time_base);

				TempPacket.stream_index = ID.FormatContext->streams[0]->index;

				Result = av_interleaved_write_frame( ID.FormatContext, &TempPacket );
				if( Result < 0 )
				{
					STREAM_ERROR( WriteFailed, Result );
				}

				m_ClipLength = (double)(TempPacket.pts * ID.FormatContext->streams[0]->time_base.num) / ID.FormatContext->streams[0]->time_base.den;
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
		CloseFile(true, !m_Live || m_Isolated);
				
		avformat_free_context( ID.FormatContext );
		ID.FormatContext = nullptr;
	}

	Stream::Shutdown();
}

CameraStreamError OutputStream::CloseFile(bool Flush, bool WriteTrailer)
{
	auto& ID = *m_InternalData;

	if (Flush && !m_Passthrough)
	{
		while (true)
		{
			if (ID.CodecContext == nullptr)
			{
				return CameraStreamError::NoStreams;
			}

			//Flush
			int Result = avcodec_send_frame(ID.CodecContext, nullptr);
			if (Result == 0)
			{
				CameraStreamError StrError = SendAll();
				if (StrError != CameraStreamError::Success)
				{
					if (!(ID.FormatContext->oformat->flags & AVFMT_NOFILE) || m_FileOpened)
					{
						m_FileOpened = false;
						avio_close(ID.FormatContext->pb);
					}

					return StrError;
				}
			}
			else if (Result == AVERROR_EOF)
			{
				break;
			}
			else
			{
				STREAM_ERROR(WriteFailed, Result);
			}
		}
	}

	if (ID.FormatContext == nullptr)
	{
		return CameraStreamError::NoStreams;
	}

	if (WriteTrailer)
	{
		int Result = av_write_trailer(ID.FormatContext);
		if (Result < 0)
		{
			STREAM_ERROR(WriteFailed, Result);
		}
	}

	if( !(ID.FormatContext->oformat->flags& AVFMT_NOFILE) || m_FileOpened )
	{
		m_FileOpened = false;
		int Result = avio_close( ID.FormatContext->pb );
		if( Result < 0 )
		{
			STREAM_ERROR( WriteFailed, Result );
		}
	}


	return CameraStreamError::Success;
}

CameraStreamError OutputStream::GenerateInitSegment(const std::string& InitSegmentPath)
{/*
	// Ensure we've initialized the format context and written the header:
	CameraStreamError InitError = Initialize();
	if (InitError != CameraStreamError::Success) {
		return InitError;
	}

	auto& ID = *m_InternalData;

	// At this point, avformat_write_header() should have already been called inside Initialize().
	// That call should have written out the ftyp/moov boxes for the initialization.
	// If we are using in-memory mode, we can extract that data now.
	if (!m_InMemory || !m_IOContext) {
		// Without in-memory mode, you'd have to set up a separate context
		// or another mechanism to capture the init segment.
		return CameraStreamError::UnknownError;
	}

	// Extract the buffer that contains the written data.
	size_t size = 0;
	uint8_t* buffer = m_IOContext->GetBuffer(&size);
	if (!buffer || size == 0) {
		return CameraStreamError::UnknownError;
	}

	// The buffer now should contain at least the init segment (ftyp + moov).
	// Write this buffer out to a file:
	std::ofstream outFile(InitSegmentPath, std::ios::binary);
	if (!outFile.is_open()) {
		return CameraStreamError::FileNotWriteable;
	}

	outFile.write(reinterpret_cast<char*>(buffer), size);
	outFile.close();

	// We've now exported the init segment. 
	// You can reset the memory IO if you want to start from a clean buffer for actual fragments.
	// Typically, you might want to re-init your output to start producing the moof fragments.
	*/
	return CameraStreamError::Success;
}

int OutputStream::GetStreamIndex()
{
	return StreamIndex;
}

}}
