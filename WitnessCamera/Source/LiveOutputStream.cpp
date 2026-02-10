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
	, _InitialDTS( 0 )
	, _LastWrittenDTS( AV_NOPTS_VALUE )
	, _SegmentStartDTS( 0 )
	, _CurrentSegmentDuration( 0.0 )
	, _KeyframesPerSegment(KeyframesPerSegment)
	, _SkipInitialKeyframes(1)
	, _CurrentSegmentIndex(0)
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
	OutStream->time_base = InID.FormatContext->streams[0]->time_base;

	// Set up in-memory I/O
	SetupMemoryIO();

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	if (Packet->flags & AV_PKT_FLAG_KEY)
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

	if (Packet->flags & AV_PKT_FLAG_KEY)
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

	// Normalize timestamps: subtract the initial DTS so the stream starts at 0.
	// This handles cameras (e.g. Tapo) whose RTSP streams start with large DTS values.
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

	// Guard against negative timestamps from B-frame reordering at stream start
	if (PacketCopy.dts != AV_NOPTS_VALUE && PacketCopy.dts < 0)
	{
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}

	// Synthesize missing PTS from DTS — some cameras (e.g. Tapo) deliver
	// packets with AV_NOPTS_VALUE for PTS, which the MP4 muxer rejects.
	if (PacketCopy.pts == AV_NOPTS_VALUE)
		PacketCopy.pts = PacketCopy.dts;

	PacketCopy.stream_index = 0;
	PacketCopy.pos = -1;

	// Enforce strictly monotonic DTS — some cameras (Tapo with B-frames)
	// deliver packets with duplicate DTS values.
	if (_LastWrittenDTS != AV_NOPTS_VALUE && PacketCopy.dts <= _LastWrittenDTS)
	{
		PacketCopy.dts = _LastWrittenDTS + 1;
		if (PacketCopy.pts < PacketCopy.dts)
			PacketCopy.pts = PacketCopy.dts;
	}
	_LastWrittenDTS = PacketCopy.dts;

	// Clamp negative durations (B-frame reordering artifacts)
	if (PacketCopy.duration < 0)
		PacketCopy.duration = 0;

	AVRational TimeBase = _FormatContext->streams[0]->time_base;
	double PacketDurationSec = (double)(PacketCopy.duration * TimeBase.num) / TimeBase.den;
	_CurrentSegmentDuration += PacketDurationSec;

	// Use av_write_frame (non-interleaving) — packets arrive in DTS order
	// from the RTSP demuxer, so interleaving is unnecessary and its internal
	// reorder buffer causes spurious "non monotonically increasing dts" errors
	// with B-frame streams (e.g. Tapo cameras).
	Result = av_write_frame(_FormatContext, &PacketCopy);
	if (Result < 0)
	{
		STREAM_ERROR(WriteFailed, Result);
	}

	return CameraStreamError::Success;
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

	// Flush the current fragment into the buffer
	av_write_frame(_FormatContext, nullptr);
	avio_flush(_FormatContext->pb);

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		if (!_StreamBacklog->empty())
		{
			auto& Seg = _StreamBacklog->back();
			Seg.Data = _CurrentBuffer;
			Seg.Duration = _CurrentSegmentDuration;
			Seg.Ready = true;
		}
	}

	_CurrentBuffer.reset();
	_CurrentSegmentIndex++;
}

}}