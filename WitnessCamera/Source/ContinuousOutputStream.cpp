#include "ContinuousOutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

#include <Log.h>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <climits>

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
	, m_AudioOutStream(nullptr)
	, m_SegmentOpen(false)
	, m_SegmentStartTimestamp(0)
	, m_FirstDTS(AV_NOPTS_VALUE)
	, m_FirstTimestampUs(AV_NOPTS_VALUE)
	, m_LastWrittenDTS(AV_NOPTS_VALUE)
	, m_LastWrittenAudioDTS(AV_NOPTS_VALUE)
	, m_TimestampNormalizationAllowed(false)
	, m_LastRawVideoDTS(AV_NOPTS_VALUE)
	, m_LastNormalizedVideoDuration(0)
	, m_TimestampCorrectionRemainder(0)
	, m_RepairedVideoTimestamps(0)
	, m_TimestampNormalizationActive(false)
	, m_TimestampNormalizationRejected(false)
	, m_TimestampProbeSamples(0)
	, m_TimestampProbeOutliers(0)
	, m_TimestampProbeDeltaTicks(0)
	, m_TimestampProbeNominalTicks(0)
	, m_QualifiedVideoDuration(0)
	, m_TimestampSourceOffset(0)
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

	AVStream* inStream = inData.FormatContext->streams[inData.ChosenStreamIndex];
	result = avcodec_parameters_copy(m_OutStream->codecpar, inStream->codecpar);
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
	m_OutStream->time_base = inStream->time_base;

	if (inData.HasAudio)
	{
		AVStream* audioInStream = inData.FormatContext->streams[inData.ChosenAudioStreamIndex];
		m_AudioOutStream = avformat_new_stream(m_FormatContext, nullptr);
		if (!m_AudioOutStream || avcodec_parameters_copy(m_AudioOutStream->codecpar, audioInStream->codecpar) < 0)
		{
			avformat_free_context(m_FormatContext);
			m_FormatContext = nullptr;
			m_AudioOutStream = nullptr;
			return CameraStreamError::UnknownError;
		}
		m_AudioOutStream->codecpar->codec_tag = 0;
		m_AudioOutStream->time_base = audioInStream->time_base;
	}

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
	m_FirstTimestampUs = AV_NOPTS_VALUE;
	m_LastWrittenDTS = AV_NOPTS_VALUE;
	m_LastWrittenAudioDTS = AV_NOPTS_VALUE;
	m_LastRawVideoDTS = AV_NOPTS_VALUE;
	m_LastNormalizedVideoDuration = 0;
	m_TimestampCorrectionRemainder = 0;
	m_TimestampNormalizationActive = false;
	m_TimestampNormalizationRejected = false;
	m_TimestampProbeSamples = 0;
	m_TimestampProbeOutliers = 0;
	m_TimestampProbeDeltaTicks = 0;
	m_TimestampProbeNominalTicks = 0;
	m_QualifiedVideoDuration = 0;
	m_TimestampSourceOffset = 0;
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
	m_AudioOutStream = nullptr;

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

	const bool isVideo = packet->stream_index == (int)inData.ChosenStreamIndex;
	const bool isAudio = inData.HasAudio && packet->stream_index == inData.ChosenAudioStreamIndex;
	if (!isVideo && !isAudio)
		return CameraStreamError::Success;
	// Do not open or split a segment on an untimed video keyframe. Following
	// inter-frames are not independently decodable without that RAP.
	if (isVideo && packet->dts == AV_NOPTS_VALUE)
		return CameraStreamError::Success;

	// Get the stream timebase from the input format context (NOT inData.Timebase which is unset on InputStream)
	AVRational inputTimebase = inData.FormatContext->streams[packet->stream_index]->time_base;

	bool isKeyframe = isVideo && (packet->flags & AV_PKT_FLAG_KEY) != 0;

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
		if (!isVideo || !isKeyframe)
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
	if (pktCopy.dts == AV_NOPTS_VALUE)
	{
		// AAC is not reordered and some RTSP sources provide only PTS. Never use
		// presentation order as decode order for video; skip an untimed video packet.
		if (!isAudio || pktCopy.pts == AV_NOPTS_VALUE)
		{
			av_packet_unref(&pktCopy);
			return CameraStreamError::Success;
		}
		pktCopy.dts = pktCopy.pts;
	}
	if (pktCopy.pts == AV_NOPTS_VALUE)
		pktCopy.pts = pktCopy.dts;

	int64_t packetTimestampUs = av_rescale_q(pktCopy.dts, inputTimebase, AV_TIME_BASE_Q);
	if (m_FirstTimestampUs == AV_NOPTS_VALUE && isVideo)
		m_FirstTimestampUs = packetTimestampUs;
	if (m_FirstTimestampUs == AV_NOPTS_VALUE || packetTimestampUs < m_FirstTimestampUs)
	{
		av_packet_unref(&pktCopy);
		return CameraStreamError::Success;
	}
	pktCopy.dts = av_rescale_q(packetTimestampUs - m_FirstTimestampUs, AV_TIME_BASE_Q, inputTimebase);
	pktCopy.pts = av_rescale_q(av_rescale_q(pktCopy.pts, inputTimebase, AV_TIME_BASE_Q) - m_FirstTimestampUs, AV_TIME_BASE_Q, inputTimebase);

	pktCopy.pos = -1;
	pktCopy.stream_index = isAudio ? m_AudioOutStream->index : m_OutStream->index;
	if (pktCopy.duration < 0 || pktCopy.duration > INT_MAX)
		pktCopy.duration = 0;
	const bool timestampRepaired = isVideo && NormalizeVideoTimestamp(&pktCopy, inputTimebase);

	// Drop non-monotonic DTS
	const int64_t lastStreamDTS = isAudio ? m_LastWrittenAudioDTS : m_LastWrittenDTS;
	if (lastStreamDTS != AV_NOPTS_VALUE && pktCopy.dts <= lastStreamDTS)
	{
		av_packet_unref(&pktCopy);
		return CameraStreamError::Success;
	}
	if (isVideo)
		m_LastWrittenDTS = pktCopy.dts;
	else
		m_LastWrittenAudioDTS = pktCopy.dts;

	// Clamp negative durations
	if (pktCopy.duration < 0)
		pktCopy.duration = 0;

	// Track segment duration using input stream timebase
	if (isVideo)
		m_SegmentDuration = (double)(pktCopy.dts * inputTimebase.num) / inputTimebase.den;

	// Log progress periodically (every ~30 seconds)
	int durationInt = (int)m_SegmentDuration;
	if (durationInt > 0 && durationInt % 30 == 0 && isKeyframe)
	{
		LOG_INFO("ContinuousOutputStream: Camera %d segment at %ds / %ds", m_CameraUID, durationInt, m_TargetSegmentDuration);
	}

	// Rescale timestamps to output timebase
	av_packet_rescale_ts(&pktCopy, inputTimebase,
		isAudio ? m_AudioOutStream->time_base : m_OutStream->time_base);

	result = av_interleaved_write_frame(m_FormatContext, &pktCopy);
	if (result < 0)
	{
		av_packet_unref(&pktCopy);
		return CameraStreamError::WriteFailed;
	}
	if (timestampRepaired)
	{
		++m_RepairedVideoTimestamps;
		if (m_RepairedVideoTimestamps <= 5 || (m_RepairedVideoTimestamps % 500) == 0)
			LOG_WARNING("ContinuousOutputStream: Camera %d repaired jittery video timestamps (%llu packets)",
				m_CameraUID, (unsigned long long)m_RepairedVideoTimestamps);
	}

	// Check if we've exceeded target duration — if so, wait for next keyframe
	if (!m_WaitingForKeyframe && m_SegmentDuration >= m_TargetSegmentDuration)
	{
		m_WaitingForKeyframe = true;
	}

	return CameraStreamError::Success;
}

bool ContinuousOutputStream::NormalizeVideoTimestamp(AVPacket* packet, AVRational inputTimebase)
{
	if (!m_TimestampNormalizationAllowed || !m_InputStream || !packet ||
		m_InputStream->GetData().CodecContext->has_b_frames != 0)
		return false;

	const int64_t rawDTS = packet->dts;
	const AVRational framerate = m_InputStream->GetData().CodecContext->framerate;
	int64_t nominalDuration = 0;
	if (framerate.num > 0 && framerate.den > 0)
	{
		const double framesPerSecond = av_q2d(framerate);
		if (framesPerSecond >= 1.0 && framesPerSecond <= 120.0)
			nominalDuration = av_rescale_q(1, av_inv_q(framerate), inputTimebase);
	}
	if (nominalDuration <= 0 && packet->duration > 0 && packet->duration <= INT_MAX)
		nominalDuration = packet->duration;
	if (nominalDuration > INT_MAX)
		nominalDuration = 0;

	const int64_t probeDuration = m_TimestampNormalizationActive ?
		m_QualifiedVideoDuration : nominalDuration;
	if (m_LastRawVideoDTS != AV_NOPTS_VALUE && probeDuration > 0 &&
		!m_TimestampNormalizationRejected)
	{
		const int64_t delta = rawDTS - m_LastRawVideoDTS;
		const int64_t error = delta - probeDuration;
		++m_TimestampProbeSamples;
		m_TimestampProbeDeltaTicks += delta;
		m_TimestampProbeNominalTicks += probeDuration;
		if (std::abs(error) * 2 > probeDuration)
			++m_TimestampProbeOutliers;
		if (!m_TimestampNormalizationActive && m_TimestampProbeSamples >= 40)
		{
			const int64_t totalError = m_TimestampProbeDeltaTicks - m_TimestampProbeNominalTicks;
			const bool averageCadenceMatches = m_TimestampProbeDeltaTicks > 0 &&
				std::abs(totalError) * 100 <= m_TimestampProbeNominalTicks * 5;
			const bool sourceClockIsJittery =
				m_TimestampProbeOutliers * 5 >= m_TimestampProbeSamples;
			if (averageCadenceMatches && sourceClockIsJittery)
			{
				m_TimestampNormalizationActive = true;
				m_QualifiedVideoDuration = (std::max<int64_t>)(1,
					m_TimestampProbeNominalTicks / m_TimestampProbeSamples);
				LOG_INFO("ContinuousOutputStream: Camera %d enabled guarded timestamp normalization after %d samples",
					m_CameraUID, m_TimestampProbeSamples);
			}
			else if (m_TimestampProbeSamples >= 120)
			{
				m_TimestampProbeSamples = 0;
				m_TimestampProbeOutliers = 0;
				m_TimestampProbeDeltaTicks = 0;
				m_TimestampProbeNominalTicks = 0;
			}
		}
		if (m_TimestampNormalizationActive &&
			av_rescale_q(m_TimestampProbeNominalTicks, inputTimebase, AV_TIME_BASE_Q) >=
				120 * AV_TIME_BASE)
		{
			const int64_t totalError = m_TimestampProbeDeltaTicks - m_TimestampProbeNominalTicks;
			if (std::abs(totalError) * 100 > m_TimestampProbeNominalTicks * 5)
			{
				m_TimestampNormalizationActive = false;
				m_TimestampNormalizationRejected = true;
				m_TimestampSourceOffset =
					(m_LastWrittenDTS + m_LastNormalizedVideoDuration) - rawDTS;
				LOG_WARNING("ContinuousOutputStream: Camera %d disabled timestamp normalization after sustained cadence mismatch",
					m_CameraUID);
			}
			m_TimestampProbeSamples = 0;
			m_TimestampProbeOutliers = 0;
			m_TimestampProbeDeltaTicks = 0;
			m_TimestampProbeNominalTicks = 0;
		}
	}

	if (m_LastWrittenDTS == AV_NOPTS_VALUE)
	{
		packet->pts = packet->dts;
		if (packet->duration <= 0 && nominalDuration > 0)
			packet->duration = nominalDuration;
		m_LastRawVideoDTS = rawDTS;
		m_LastNormalizedVideoDuration = packet->duration > 0 ? packet->duration : 1;
		return false;
	}

	const int64_t stepDuration = m_TimestampNormalizationActive ?
		m_QualifiedVideoDuration : (nominalDuration > 0 ? nominalDuration : m_LastNormalizedVideoDuration);
	const int64_t expectedDTS = m_LastWrittenDTS + (std::max<int64_t>)(1, m_LastNormalizedVideoDuration);
	if (!m_TimestampNormalizationActive)
	{
		const int64_t adjustedRawDTS = rawDTS + m_TimestampSourceOffset;
		const bool repaired = adjustedRawDTS <= m_LastWrittenDTS;
		if (repaired)
			packet->dts = expectedDTS;
		else
			packet->dts = adjustedRawDTS;
		packet->pts = packet->dts;
		if (packet->duration <= 0 && stepDuration > 0)
			packet->duration = stepDuration;
		m_LastRawVideoDTS = rawDTS;
		m_LastNormalizedVideoDuration = packet->duration > 0 ? packet->duration : 1;
		return repaired;
	}

	const int64_t stableDuration = (std::min<int64_t>)(INT_MAX,
		(std::max<int64_t>)(1, stepDuration));
	const int64_t phaseError = rawDTS - expectedDTS;
	const int64_t responseWindow = av_rescale_q(10 * AV_TIME_BASE, AV_TIME_BASE_Q, inputTimebase);
	const int64_t maxCorrection = (std::min)(
		stableDuration / 20, (int64_t)INT_MAX - stableDuration);
	int64_t correction = 0;
	if (responseWindow > 0)
	{
		const int64_t phaseLimit = (std::max<int64_t>)(1, responseWindow / 10);
		const int64_t boundedError = (std::max)(-phaseLimit, (std::min)(phaseError, phaseLimit));
		const int64_t numerator = boundedError * stableDuration + m_TimestampCorrectionRemainder;
		correction = numerator / responseWindow;
		m_TimestampCorrectionRemainder = numerator % responseWindow;
	}
	correction = (std::max)(-maxCorrection, (std::min)(correction, maxCorrection));
	packet->dts = expectedDTS;
	packet->pts = expectedDTS;
	packet->duration = (std::max<int64_t>)(1, stableDuration + correction);
	m_LastRawVideoDTS = rawDTS;
	m_LastNormalizedVideoDuration = packet->duration;
	return rawDTS != expectedDTS;
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
