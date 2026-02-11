#include "LiveOutputStream.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

#include <sstream>
#include <chrono>

namespace Witness{
namespace Camera{

static const int AVIOBufferSize = 64 * 1024;

LiveOutputStream::LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment)
	: Stream()
	, _LiveCachePath( LiveCachePath )
	, _StreamBacklog( new std::vector<LiveStreamSegment>() )
	, _InputStream(InputStream)
	, _FormatContext( nullptr )
	, _AVIOContext( nullptr )
	, _AVIOBuffer( nullptr )
	, _HeaderWritten( false )
	, _InitSegmentCaptured( false )
	, _HasInitialDTS( false )
	, _HasAudioStream( false )
	, _InitialDTS( 0 )
	, _InitialAudioDTS( AV_NOPTS_VALUE )
	, _LastWrittenDTS( AV_NOPTS_VALUE )
	, _SegmentStartDTS( 0 )
	, _CurrentSegmentDuration( 0.0 )
	, _SkipInitialKeyframes(1)
	, _AudioInputStreamIndex(-1)
	, _CurrentSegmentIndex(0)
	, _CurrentPartialIndex(0)
	, _PartialStartDTS(AV_NOPTS_VALUE)
	, _CurrentPartialDuration(0.0)
	, _PartialTargetDuration(0.33)
	, _CurrentPartialIsIndependent(false)
	, _PartialBufferOffset(0)
	, _SegmentsMutex( new std::mutex )
{
}

LiveOutputStream::~LiveOutputStream()
{
	Shutdown();

	delete _StreamBacklog;
	_StreamBacklog = nullptr;

	delete _SegmentsMutex;
	_SegmentsMutex = nullptr;
}

CameraStreamError LiveOutputStream::Initialize()
{
	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::ProcessFrame(const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream)
{
	return CameraStreamError::Success;
}

void LiveOutputStream::Shutdown()
{
	if (_FormatContext)
	{
		if (_FormatContext->pb)
		{
			av_write_frame(_FormatContext, nullptr);
			avio_flush(_FormatContext->pb);
		}

		// Detach our custom AVIO so avformat doesn't try to close it
		_FormatContext->pb = nullptr;

		avformat_free_context(_FormatContext);
		_FormatContext = nullptr;
	}

	if (_AVIOContext)
	{
		// Buffer is owned by us, not av_free'd by avio
		av_free(_AVIOContext);
		_AVIOContext = nullptr;
	}

	if (_AVIOBuffer)
	{
		av_free(_AVIOBuffer);
		_AVIOBuffer = nullptr;
	}

	_CurrentBuffer.reset();
}

void LiveOutputStream::ResetForReconnect(InputStream* NewInputStream)
{
	// Tear down FFmpeg state but keep segments/init so HLS.js doesn't lose its reference.
	// Don't flush — the partial segment data is incomplete and the muxer may be in an
	// inconsistent state (e.g. audio stream created but no audio packets written yet).
	if (_FormatContext)
	{
		_FormatContext->pb = nullptr;
		avformat_free_context(_FormatContext);
		_FormatContext = nullptr;
	}

	if (_AVIOContext)
	{
		av_free(_AVIOContext);
		_AVIOContext = nullptr;
	}

	if (_AVIOBuffer)
	{
		av_free(_AVIOBuffer);
		_AVIOBuffer = nullptr;
	}

	_CurrentBuffer.reset();
	_InputStream = NewInputStream;
	_HeaderWritten = false;
	_InitSegmentCaptured = false;
	_HasInitialDTS = false;
	_HasAudioStream = false;
	_InitialDTS = 0;
	_InitialAudioDTS = AV_NOPTS_VALUE;
	_LastWrittenDTS = AV_NOPTS_VALUE;
	_AudioInputStreamIndex = -1;
	_PartialBufferOffset = 0;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialIsIndependent = false;
	_SkipInitialKeyframes = 1;

	// Remove any incomplete segment from the backlog — it will never be finished
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		if (!_StreamBacklog->empty() && !_StreamBacklog->back().Ready)
		{
			_StreamBacklog->pop_back();
		}
	}
}

int LiveOutputStream::WriteBuffer(void* Opaque, const uint8_t* Buffer, int BufferSize)
{
	LiveOutputStream* Self = static_cast<LiveOutputStream*>(Opaque);

	if (Self->_CurrentBuffer && BufferSize > 0)
	{
		Self->_CurrentBuffer->insert(Self->_CurrentBuffer->end(), Buffer, Buffer + BufferSize);
	}

	return BufferSize;
}

int64_t LiveOutputStream::SeekBuffer(void* Opaque, int64_t Offset, int Origin)
{
	// AVSEEK_SIZE: return total buffer size
	if (Origin == AVSEEK_SIZE)
	{
		LiveOutputStream* Self = static_cast<LiveOutputStream*>(Opaque);
		return Self->_CurrentBuffer ? (int64_t)Self->_CurrentBuffer->size() : 0;
	}

	// For fragmented MP4 with frag_custom, seeks shouldn't happen on the
	// output side after the header. Return 0 to indicate unseekable.
	return -1;
}

void LiveOutputStream::SetupMemoryIO()
{
	_AVIOBuffer = (uint8_t*)av_malloc(AVIOBufferSize);

	_AVIOContext = avio_alloc_context(
		_AVIOBuffer,
		AVIOBufferSize,
		1, // writable
		this,
		nullptr, // no read
		&LiveOutputStream::WriteBuffer,
		&LiveOutputStream::SeekBuffer
	);

	_AVIOContext->seekable = 0;

	_FormatContext->pb = _AVIOContext;
	_FormatContext->flags |= AVFMT_FLAG_CUSTOM_IO;
}

CameraStreamError LiveOutputStream::InitFormatContext()
{
	if (_FormatContext)
	{
		return CameraStreamError::Success;
	}

	_InputStream->Initialize();

	auto& InID = _InputStream->GetData();
	if (!InID.CodecContext)
	{
		STREAM_ERROR(NoStreamInput, 0);
	}

	int Result = avformat_alloc_output_context2(&_FormatContext, nullptr, "mp4", nullptr);
	if (Result < 0 || !_FormatContext)
	{
		STREAM_ERROR(UnknownError, Result);
	}

	AVStream* OutStream = avformat_new_stream(_FormatContext, nullptr);
	if (!OutStream)
	{
		STREAM_ERROR(UnknownError, 0);
	}

	Result = avcodec_parameters_from_context(OutStream->codecpar, InID.CodecContext);
	if (Result < 0)
	{
		STREAM_ERROR(DecoderReceiverError, Result);
	}

	// Remap deprecated pixel formats
	switch (OutStream->codecpar->format)
	{
	case AV_PIX_FMT_YUVJ420P: OutStream->codecpar->format = AV_PIX_FMT_YUV420P; break;
	case AV_PIX_FMT_YUVJ422P: OutStream->codecpar->format = AV_PIX_FMT_YUV422P; break;
	case AV_PIX_FMT_YUVJ444P: OutStream->codecpar->format = AV_PIX_FMT_YUV444P; break;
	case AV_PIX_FMT_YUVJ440P: OutStream->codecpar->format = AV_PIX_FMT_YUV440P; break;
	}

	OutStream->codecpar->codec_tag = 0;
	OutStream->time_base = InID.FormatContext->streams[InID.ChosenStreamIndex]->time_base;

	// Add audio stream if available and codec is supported in MP4
	_HasAudioStream = false;
	_AudioInputStreamIndex = -1;
	if (InID.HasAudio && InID.ChosenAudioStreamIndex >= 0)
	{
		AVStream* AudioInStream = InID.FormatContext->streams[InID.ChosenAudioStreamIndex];
		AVCodecID AudioCodec = AudioInStream->codecpar->codec_id;

		// Only add audio if the codec is supported in MP4 container.
		// PCM/G.711 variants (pcm_alaw, pcm_mulaw) from cameras like
		// Hikvision are not supported in fragmented MP4.
		bool AudioCodecSupported =
			AudioCodec == AV_CODEC_ID_AAC ||
			AudioCodec == AV_CODEC_ID_MP3 ||
			AudioCodec == AV_CODEC_ID_AC3 ||
			AudioCodec == AV_CODEC_ID_EAC3 ||
			AudioCodec == AV_CODEC_ID_FLAC ||
			AudioCodec == AV_CODEC_ID_OPUS;

		if (AudioCodecSupported)
		{
			AVStream* AudioOutStream = avformat_new_stream(_FormatContext, nullptr);
			if (AudioOutStream)
			{
				Result = avcodec_parameters_copy(AudioOutStream->codecpar, AudioInStream->codecpar);
				if (Result >= 0)
				{
					AudioOutStream->codecpar->codec_tag = 0;
					AudioOutStream->time_base = AudioInStream->time_base;
					_HasAudioStream = true;
					_AudioInputStreamIndex = InID.ChosenAudioStreamIndex;
				}
			}
		}
	}

	// Set up in-memory I/O
	SetupMemoryIO();

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	bool IsAudioPacket = _HasAudioStream && Packet->stream_index == _AudioInputStreamIndex;

	// Skip initial keyframes only for video
	if (!IsAudioPacket && (Packet->flags & AV_PKT_FLAG_KEY))
	{
		if (_SkipInitialKeyframes > 0)
		{
			_SkipInitialKeyframes--;
			return CameraStreamError::Success;
		}
	}

	if (!_FormatContext)
	{
		CameraStreamError Result = InitFormatContext();
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}

	// Segment boundaries are driven by video keyframes only
	if (!IsAudioPacket && (Packet->flags & AV_PKT_FLAG_KEY))
	{
		if (_HeaderWritten)
		{
			FinishCurrentSegment();
		}

		CameraStreamError Result = StartNewSegment(Packet);
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}

	if (!_HeaderWritten || !_CurrentBuffer)
	{
		return CameraStreamError::Success;
	}

	AVPacket PacketCopy;
	memset(&PacketCopy, 0, sizeof(PacketCopy));
	int Result = av_packet_ref(&PacketCopy, Packet);
	if (Result < 0)
	{
		STREAM_ERROR(RefError, Result);
	}

	if (IsAudioPacket)
	{
		// Audio timestamp normalization — independent from video
		if (_InitialAudioDTS == AV_NOPTS_VALUE && PacketCopy.dts != AV_NOPTS_VALUE)
		{
			_InitialAudioDTS = PacketCopy.dts;
		}

		if (_InitialAudioDTS != AV_NOPTS_VALUE)
		{
			if (PacketCopy.dts != AV_NOPTS_VALUE)
				PacketCopy.dts -= _InitialAudioDTS;
			if (PacketCopy.pts != AV_NOPTS_VALUE)
				PacketCopy.pts -= _InitialAudioDTS;
		}

		if (PacketCopy.dts != AV_NOPTS_VALUE && PacketCopy.dts < 0)
		{
			av_packet_unref(&PacketCopy);
			return CameraStreamError::Success;
		}

		// Rescale from input audio timebase to output audio timebase
		auto& InID = _InputStream->GetData();
		AVRational InTimeBase = InID.FormatContext->streams[InID.ChosenAudioStreamIndex]->time_base;
		AVRational OutTimeBase = _FormatContext->streams[1]->time_base;
		PacketCopy.dts = av_rescale_q(PacketCopy.dts, InTimeBase, OutTimeBase);
		PacketCopy.pts = av_rescale_q(PacketCopy.pts, InTimeBase, OutTimeBase);
		PacketCopy.duration = av_rescale_q(PacketCopy.duration, InTimeBase, OutTimeBase);

		// Clamp negative durations from timebase conversion rounding
		if (PacketCopy.duration < 0)
			PacketCopy.duration = 0;

		if (PacketCopy.pts == AV_NOPTS_VALUE)
			PacketCopy.pts = PacketCopy.dts;

		PacketCopy.stream_index = 1;
		PacketCopy.pos = -1;
	}
	else
	{
		// Video timestamp normalization
		if (!_HasInitialDTS && PacketCopy.dts != AV_NOPTS_VALUE)
		{
			_InitialDTS = PacketCopy.dts;
			_HasInitialDTS = true;
		}

		if (_HasInitialDTS)
		{
			if (PacketCopy.dts != AV_NOPTS_VALUE)
				PacketCopy.dts -= _InitialDTS;
			if (PacketCopy.pts != AV_NOPTS_VALUE)
				PacketCopy.pts -= _InitialDTS;
		}

		if (PacketCopy.dts != AV_NOPTS_VALUE && PacketCopy.dts < 0)
		{
			av_packet_unref(&PacketCopy);
			return CameraStreamError::Success;
		}

		// Force PTS = DTS for live HLS passthrough (B-frame workaround)
		PacketCopy.pts = PacketCopy.dts;

		PacketCopy.stream_index = 0;
		PacketCopy.pos = -1;

		// Drop packets with non-monotonic DTS (B-frame streams)
		if (_LastWrittenDTS != AV_NOPTS_VALUE && PacketCopy.dts <= _LastWrittenDTS)
		{
			av_packet_unref(&PacketCopy);
			return CameraStreamError::Success;
		}
		_LastWrittenDTS = PacketCopy.dts;

		if (PacketCopy.duration < 0)
			PacketCopy.duration = 0;

		// Track segment/partial duration from video packets only
		AVRational TimeBase = _FormatContext->streams[0]->time_base;
		double PacketDurationSec = (double)(PacketCopy.duration * TimeBase.num) / TimeBase.den;
		_CurrentSegmentDuration += PacketDurationSec;
		_CurrentPartialDuration += PacketDurationSec;

		if (PacketCopy.flags & AV_PKT_FLAG_KEY)
			_CurrentPartialIsIndependent = true;
	}

	Result = av_write_frame(_FormatContext, &PacketCopy);
	av_packet_unref(&PacketCopy);
	if (Result < 0)
	{
		STREAM_ERROR(WriteFailed, Result);
	}

	// Flush partial segments based on video duration only
	if (!IsAudioPacket && _CurrentPartialDuration >= _PartialTargetDuration)
	{
		FlushPartialSegment(_CurrentPartialIsIndependent);
	}

	return CameraStreamError::Success;
}

void LiveOutputStream::FlushPartialSegment(bool IsIndependent)
{
	if (!_FormatContext || !_CurrentBuffer)
		return;

	// Flush current fragment data into the buffer
	av_write_frame(_FormatContext, nullptr);
	avio_flush(_FormatContext->pb);

	// Only create a partial if we actually accumulated data since the last flush
	size_t CurrentSize = _CurrentBuffer->size();
	if (CurrentSize <= _PartialBufferOffset || _CurrentPartialDuration <= 0.0)
		return;

	// Create a partial that references the byte range [_PartialBufferOffset, CurrentSize)
	// within the single segment buffer
	auto PartialData = std::make_shared<std::vector<uint8_t>>(
		_CurrentBuffer->begin() + _PartialBufferOffset,
		_CurrentBuffer->begin() + CurrentSize
	);

	_PartialBufferOffset = CurrentSize;

	LiveStreamPartialSegment Partial;
	Partial.Data = PartialData;
	Partial.Duration = _CurrentPartialDuration;
	Partial.PartIndex = _CurrentPartialIndex;
	Partial.Independent = IsIndependent;

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		if (!_StreamBacklog->empty())
		{
			_StreamBacklog->back().Partials.push_back(Partial);
		}
	}

	_CurrentPartialIndex++;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialIsIndependent = false;
}

CameraStreamError LiveOutputStream::StartNewSegment(const AVPacket* Packet)
{
	if (!_InitSegmentCaptured)
	{
		// Write the init segment to an in-memory buffer
		_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

		AVDictionary* options = nullptr;
		av_dict_set(&options, "movflags", "empty_moov+frag_custom+dash+default_base_moof+negative_cts_offsets", 0);
		av_dict_set(&options, "brand", "iso6", 0);

		int Result = avformat_write_header(_FormatContext, &options);
		av_dict_free(&options);
		if (Result < 0)
		{
			STREAM_ERROR(WriteFailed, Result);
		}

		_HeaderWritten = true;

		avio_flush(_FormatContext->pb);

		// Store the init segment (ftyp + moov)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			_InitSegmentData = _CurrentBuffer;
		}

		_InitSegmentCaptured = true;
	}

	// Start a fresh buffer for this segment
	_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

	_SegmentStartDTS = Packet->dts;
	_CurrentSegmentDuration = 0.0;
	_CurrentPartialIndex = 0;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialIsIndependent = true; // first partial starts with keyframe
	_PartialBufferOffset = 0;
	_CurrentSegmentWallTime = std::chrono::system_clock::now();

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		// Trim old segments
		while (_StreamBacklog->size() >= 5)
		{
			_StreamBacklog->erase(_StreamBacklog->begin());
		}

		LiveStreamSegment NewSegment;
		NewSegment.Data = nullptr; // Will be set when finished
		NewSegment.Duration = 0.0;
		NewSegment.SegmentIndex = _CurrentSegmentIndex;
		NewSegment.SegmentTime = _CurrentSegmentWallTime;
		NewSegment.Ready = false;

		_StreamBacklog->push_back(NewSegment);
	}

	return CameraStreamError::Success;
}

void LiveOutputStream::FinishCurrentSegment()
{
	if (!_FormatContext || !_CurrentBuffer)
	{
		return;
	}

	// Flush remaining data as the final partial of this segment
	FlushPartialSegment(_CurrentPartialIsIndependent);

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		if (!_StreamBacklog->empty())
		{
			auto& Seg = _StreamBacklog->back();
			Seg.Data = _CurrentBuffer;
			Seg.Duration = _CurrentSegmentDuration;
			Seg.Ready = true;

			// Release partial data buffers — the full segment contains
			// all the data. Keep Duration/PartIndex/Independent for the
			// playlist EXT-X-PART tags but free the actual byte buffers.
			for (auto& Partial : Seg.Partials)
			{
				Partial.Data.reset();
			}
		}
	}

	_CurrentBuffer.reset();
	_CurrentSegmentIndex++;
}

}}