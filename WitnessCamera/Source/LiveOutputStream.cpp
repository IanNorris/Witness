#include "LiveOutputStream.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

#include <sstream>
#include <chrono>
#include <format>
#include <filesystem>

namespace Witness{
namespace Camera{

LiveOutputStream::LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment)
	: Stream()
	, _LiveCachePath( LiveCachePath )
	, _StreamBacklog( new std::vector<LiveStreamSegment>() )
	, _InputStream(InputStream)
	, _FormatContext( nullptr )
	, _CodecContext( nullptr )
	, _HeaderWritten( false )
	, _InitSegmentCaptured( false )
	, _FirstPacketSeen( false )
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
			// Flush any remaining fragment data
			av_write_frame(_FormatContext, nullptr);
			avio_flush(_FormatContext->pb);
			avio_closep(&_FormatContext->pb);
		}

		avformat_free_context(_FormatContext);
		_FormatContext = nullptr;
	}

	if (_CodecContext)
	{
		avcodec_free_context(&_CodecContext);
		_CodecContext = nullptr;
	}
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

	// Create format context for fragmented MP4 output
	int Result = avformat_alloc_output_context2(&_FormatContext, nullptr, "mp4", nullptr);
	if (Result < 0 || !_FormatContext)
	{
		STREAM_ERROR(UnknownError, Result);
	}

	// Create output stream by copying codec parameters from input (remux, no encode)
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

	// Copy the input stream's timebase so timestamps pass through unchanged
	OutStream->time_base = InID.FormatContext->streams[0]->time_base;

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	// Skip the first keyframe(s) to let the decoder stabilize
	if (Packet->flags & AV_PKT_FLAG_KEY)
	{
		if (_SkipInitialKeyframes > 0)
		{
			_SkipInitialKeyframes--;
			return CameraStreamError::Success;
		}
	}

	// Lazy-init the format context on first usable packet
	if (!_FormatContext)
	{
		CameraStreamError Result = InitFormatContext();
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}

	// On keyframe: finish current segment, start new one
	if (Packet->flags & AV_PKT_FLAG_KEY)
	{
		if (_HeaderWritten)
		{
			// Flush the current fragment and close the segment file
			FinishCurrentSegment();
		}

		CameraStreamError Result = StartNewSegment(Packet);
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}

	// Don't write until we have an open segment
	if (!_HeaderWritten || !_FormatContext->pb)
	{
		return CameraStreamError::Success;
	}

	// Copy the packet and pass timestamps through unchanged
	AVPacket PacketCopy;
	memset(&PacketCopy, 0, sizeof(PacketCopy));
	int Result = av_packet_ref(&PacketCopy, Packet);
	if (Result < 0)
	{
		STREAM_ERROR(RefError, Result);
	}

	PacketCopy.stream_index = 0;
	PacketCopy.pos = -1;

	// Track segment duration using packet timestamps
	AVRational TimeBase = _FormatContext->streams[0]->time_base;
	double PacketDurationSec = (double)(PacketCopy.duration * TimeBase.num) / TimeBase.den;
	_CurrentSegmentDuration += PacketDurationSec;

	Result = av_interleaved_write_frame(_FormatContext, &PacketCopy);
	if (Result < 0)
	{
		STREAM_ERROR(WriteFailed, Result);
	}

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::StartNewSegment(const AVPacket* Packet)
{
	CreateDirectoryA(_LiveCachePath.c_str(), nullptr);

	if (!_InitSegmentCaptured)
	{
		// Write the init segment (ftyp + moov with empty sample tables).
		// empty_moov: writes ftyp+moov immediately in avformat_write_header
		// frag_custom: we control fragment boundaries via av_write_frame(NULL)
		// dash: fragments start with styp instead of ftyp
		// default_base_moof: CMAF compliance (moof-relative offsets in trun)
		// negative_cts_offsets: CTTS v1 instead of edit lists (no edts/elst)
		std::stringstream InitPath;
		InitPath << _LiveCachePath << "\\Live_" << _InputStream->GetSourceId() << "_Init.mp4";
		_InitSegmentPath = InitPath.str();

		int Result = avio_open(&_FormatContext->pb, _InitSegmentPath.c_str(), AVIO_FLAG_WRITE);
		if (Result < 0)
		{
			STREAM_ERROR(FileNotWriteable, Result);
		}

		AVDictionary* options = nullptr;
		av_dict_set(&options, "movflags", "empty_moov+frag_custom+dash+default_base_moof+negative_cts_offsets", 0);
		av_dict_set(&options, "brand", "iso6", 0);

		Result = avformat_write_header(_FormatContext, &options);
		av_dict_free(&options);
		if (Result < 0)
		{
			STREAM_ERROR(WriteFailed, Result);
		}

		_HeaderWritten = true;

		// Init segment is now written (ftyp + moov). Close the file.
		avio_flush(_FormatContext->pb);
		avio_closep(&_FormatContext->pb);

		_InitSegmentCaptured = true;
	}

	// Open a new file for this segment
	std::stringstream SegPath;
	SegPath << _LiveCachePath << "\\Live_" << _InputStream->GetSourceId() << "_" << _CurrentSegmentIndex << ".mp4";
	std::string SegmentPath = SegPath.str();

	int Result = avio_open(&_FormatContext->pb, SegmentPath.c_str(), AVIO_FLAG_WRITE);
	if (Result < 0)
	{
		STREAM_ERROR(FileNotWriteable, Result);
	}

	_SegmentStartDTS = Packet->dts;
	_CurrentSegmentDuration = 0.0;
	_CurrentSegmentWallTime = std::chrono::system_clock::now();

	// Add segment entry to backlog
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		// Trim old segments
		while (_StreamBacklog->size() >= 5)
		{
			// Delete the old segment file
			auto& OldSeg = _StreamBacklog->front();
			std::error_code ec;
			std::filesystem::remove(OldSeg.FilePath, ec);
			_StreamBacklog->erase(_StreamBacklog->begin());
		}

		LiveStreamSegment NewSegment;
		NewSegment.FilePath = SegmentPath;
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
	if (!_FormatContext || !_FormatContext->pb)
	{
		return;
	}

	// Flush the current fragment
	av_write_frame(_FormatContext, nullptr);
	avio_flush(_FormatContext->pb);
	avio_closep(&_FormatContext->pb);

	// Mark the segment as ready with its final duration
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		if (!_StreamBacklog->empty())
		{
			auto& Seg = _StreamBacklog->back();
			Seg.Duration = _CurrentSegmentDuration;
			Seg.Ready = true;
		}
	}

	_CurrentSegmentIndex++;
}

}}