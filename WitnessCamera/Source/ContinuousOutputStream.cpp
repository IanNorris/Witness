#include "ContinuousOutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

#include <Log.h>
#include <filesystem>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace fs = std::filesystem;

namespace Witness{
namespace Camera{

ContinuousOutputStream::ContinuousOutputStream(const std::string& basePath, int cameraUID, InputStream* inputStream)
	: m_BasePath(basePath)
	, m_CameraUID(cameraUID)
	, m_InputStream(inputStream)
	, m_FormatContext(nullptr)
	, m_OutStream(nullptr)
	, m_SegmentOpen(false)
	, m_SegmentStartTimestamp(0)
	, m_FirstDTS(AV_NOPTS_VALUE)
	, m_LastWrittenDTS(AV_NOPTS_VALUE)
	, m_SegmentDuration(0.0)
	, m_TargetSegmentDuration(300) // 5 minutes
	, m_WaitingForKeyframe(false)
	, m_OnSegmentComplete(nullptr)
{
	std::memset(m_ErrorMessage, 0, sizeof(m_ErrorMessage));

	// Ensure output directory exists
	std::error_code ec;
	fs::create_directories(m_BasePath, ec);
	if (ec)
	{
		LOG_ERROR("ContinuousOutputStream: Failed to create directory %s: %s", m_BasePath.c_str(), ec.message().c_str());
	}
}

ContinuousOutputStream::~ContinuousOutputStream()
{
	Finalize();
}

void ContinuousOutputStream::SetSegmentCompleteCallback(SegmentCompleteCallback callback)
{
	m_OnSegmentComplete = std::move(callback);
}

void ContinuousOutputStream::SetTargetSegmentDuration(int seconds)
{
	m_TargetSegmentDuration = seconds;
}

CameraStreamError ContinuousOutputStream::StartNewSegment()
{
	if (m_SegmentOpen)
	{
		CameraStreamError err = FinalizeCurrentSegment();
		if (err != CameraStreamError::Success)
			return err;
	}

	auto& inData = m_InputStream->GetData();
	if (!inData.CodecContext)
		return CameraStreamError::NoStreamInput;

	// Generate filename from current unix timestamp
	auto now = std::chrono::system_clock::now();
	m_SegmentStartTimestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

	std::string filename = std::to_string(m_SegmentStartTimestamp) + ".mp4";
	std::string filepath = (fs::path(m_BasePath) / filename).string();

	int result = avformat_alloc_output_context2(&m_FormatContext, nullptr, nullptr, filepath.c_str());
	if (result < 0 || !m_FormatContext)
	{
		LOG_ERROR("ContinuousOutputStream: Failed to create output context for %s", filepath.c_str());
		return CameraStreamError::UnknownError;
	}

	// Create video stream — passthrough mode (no encoder needed).
	// Copy codec parameters directly from the input stream so any codec
	// (H.264, HEVC, etc.) works even without the corresponding encoder library.
	m_OutStream = avformat_new_stream(m_FormatContext, nullptr);
	if (!m_OutStream)
	{
		avformat_free_context(m_FormatContext);
		m_FormatContext = nullptr;
		return CameraStreamError::UnknownError;
	}

	result = avcodec_parameters_from_context(m_OutStream->codecpar, inData.CodecContext);
	if (result < 0)
	{
		avformat_free_context(m_FormatContext);
		m_FormatContext = nullptr;
		return CameraStreamError::UnknownError;
	}

	// Remap deprecated pixel formats
	switch (m_OutStream->codecpar->format)
	{
	case AV_PIX_FMT_YUVJ420P: m_OutStream->codecpar->format = AV_PIX_FMT_YUV420P; break;
	case AV_PIX_FMT_YUVJ422P: m_OutStream->codecpar->format = AV_PIX_FMT_YUV422P; break;
	case AV_PIX_FMT_YUVJ444P: m_OutStream->codecpar->format = AV_PIX_FMT_YUV444P; break;
	case AV_PIX_FMT_YUVJ440P: m_OutStream->codecpar->format = AV_PIX_FMT_YUV440P; break;
	}

	m_OutStream->codecpar->codec_tag = 0; // Let muxer choose
	m_OutStream->time_base = inData.FormatContext->streams[inData.ChosenStreamIndex]->time_base;

	// Open output file
	if (!(m_FormatContext->oformat->flags & AVFMT_NOFILE))
	{
		result = avio_open(&m_FormatContext->pb, filepath.c_str(), AVIO_FLAG_WRITE);
		if (result < 0)
		{
			LOG_ERROR("ContinuousOutputStream: Failed to open file %s", filepath.c_str());
			avformat_free_context(m_FormatContext);
			m_FormatContext = nullptr;
			return CameraStreamError::UnknownError;
		}
	}

	result = avformat_write_header(m_FormatContext, nullptr);
	if (result < 0)
	{
		LOG_ERROR("ContinuousOutputStream: Failed to write header for %s", filepath.c_str());
		avio_close(m_FormatContext->pb);
		avformat_free_context(m_FormatContext);
		m_FormatContext = nullptr;
		return CameraStreamError::UnknownError;
	}

	m_SegmentOpen = true;
	m_FirstDTS = AV_NOPTS_VALUE;
	m_LastWrittenDTS = AV_NOPTS_VALUE;
	m_SegmentDuration = 0.0;
	m_WaitingForKeyframe = false;

	LOG_INFO("ContinuousOutputStream: Camera %d started segment %s (target %ds)", m_CameraUID, filepath.c_str(), m_TargetSegmentDuration);

	return CameraStreamError::Success;
}

CameraStreamError ContinuousOutputStream::FinalizeCurrentSegment()
{
	if (!m_SegmentOpen || !m_FormatContext)
		return CameraStreamError::Success;

	m_SegmentOpen = false;

	std::string filepath = m_FormatContext->url ? m_FormatContext->url : "";
	int duration = static_cast<int>(m_SegmentDuration);

	int result = av_write_trailer(m_FormatContext);
	if (result < 0)
	{
		LOG_ERROR("ContinuousOutputStream: Failed to write trailer for camera %d", m_CameraUID);
	}

	if (!(m_FormatContext->oformat->flags & AVFMT_NOFILE))
	{
		avio_close(m_FormatContext->pb);
	}

	avformat_free_context(m_FormatContext);
	m_FormatContext = nullptr;
	m_OutStream = nullptr;

	auto now = std::chrono::system_clock::now();
	int64_t endTimestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

	// Only register segments with meaningful duration
	if (duration > 0 && m_OnSegmentComplete)
	{
		m_OnSegmentComplete(m_CameraUID, m_SegmentStartTimestamp, endTimestamp, duration, filepath);
	}
	else if (duration <= 0 && !filepath.empty())
	{
		// Delete empty/tiny segments
		std::error_code ec;
		fs::remove(filepath, ec);
	}

	LOG_INFO("ContinuousOutputStream: Camera %d finalized segment (%ds)", m_CameraUID, duration);

	return CameraStreamError::Success;
}

CameraStreamError ContinuousOutputStream::WritePacket(const AVPacket* packet)
{
	if (!packet)
		return CameraStreamError::Success;

	auto& inData = m_InputStream->GetData();

	// Skip non-video packets (audio, subtitles, etc.)
	if (packet->stream_index != (int)inData.ChosenStreamIndex)
		return CameraStreamError::Success;

	// Get the stream timebase from the input format context (NOT inData.Timebase which is unset on InputStream)
	AVRational inputTimebase = inData.FormatContext->streams[inData.ChosenStreamIndex]->time_base;

	bool isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

	// If waiting for a keyframe to split on, and this is one — finalize and start new
	if (m_WaitingForKeyframe && isKeyframe)
	{
		CameraStreamError err = FinalizeCurrentSegment();
		if (err != CameraStreamError::Success)
			return err;
	}

	// Start a new segment if needed (first packet or after finalize)
	if (!m_SegmentOpen)
	{
		// Must start on a keyframe
		if (!isKeyframe)
			return CameraStreamError::Success;

		CameraStreamError err = StartNewSegment();
		if (err != CameraStreamError::Success)
			return err;
	}

	// Copy and normalize packet timestamps
	AVPacket pktCopy;
	memset(&pktCopy, 0, sizeof(pktCopy));
	int result = av_packet_ref(&pktCopy, packet);
	if (result < 0)
		return CameraStreamError::RefError;

	// Normalize DTS/PTS to start from 0
	if (m_FirstDTS == AV_NOPTS_VALUE)
	{
		m_FirstDTS = pktCopy.dts;
	}
	pktCopy.dts -= m_FirstDTS;
	pktCopy.pts -= m_FirstDTS;

	// Handle missing PTS
	if (pktCopy.pts == AV_NOPTS_VALUE)
		pktCopy.pts = pktCopy.dts;

	pktCopy.pos = -1;
	pktCopy.stream_index = 0; // We only have one stream

	// Drop non-monotonic DTS
	if (m_LastWrittenDTS != AV_NOPTS_VALUE && pktCopy.dts <= m_LastWrittenDTS)
	{
		av_packet_unref(&pktCopy);
		return CameraStreamError::Success;
	}
	m_LastWrittenDTS = pktCopy.dts;

	// Clamp negative durations
	if (pktCopy.duration < 0)
		pktCopy.duration = 0;

	// Track segment duration using input stream timebase
	m_SegmentDuration = (double)(pktCopy.dts * inputTimebase.num) / inputTimebase.den;

	// Log progress periodically (every ~30 seconds)
	int durationInt = (int)m_SegmentDuration;
	if (durationInt > 0 && durationInt % 30 == 0 && isKeyframe)
	{
		LOG_INFO("ContinuousOutputStream: Camera %d segment at %ds / %ds", m_CameraUID, durationInt, m_TargetSegmentDuration);
	}

	// Rescale timestamps to output timebase
	av_packet_rescale_ts(&pktCopy, inputTimebase, m_OutStream->time_base);

	result = av_interleaved_write_frame(m_FormatContext, &pktCopy);
	if (result < 0)
	{
		av_packet_unref(&pktCopy);
		return CameraStreamError::WriteFailed;
	}

	// Check if we've exceeded target duration — if so, wait for next keyframe
	if (!m_WaitingForKeyframe && m_SegmentDuration >= m_TargetSegmentDuration)
	{
		m_WaitingForKeyframe = true;
	}

	return CameraStreamError::Success;
}

void ContinuousOutputStream::Finalize()
{
	FinalizeCurrentSegment();
}

void ContinuousOutputStream::ResetForReconnect(InputStream* newInputStream)
{
	FinalizeCurrentSegment();
	m_InputStream = newInputStream;
}

}}
