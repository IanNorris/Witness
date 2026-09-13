#include "LiveOutputStream.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"

#include <Log.h>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <cmath>
#include <climits>

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
	, _HasBFrames( false )
	, _HasAudioStream( false )
	, _AllowTimestampNormalization( false )
	, _NormalizeNoBFrameTimestamps( false )
	, _TimestampNormalizationRejected( false )
	, _InitialDTS( 0 )
	, _InitialTimestampUs( AV_NOPTS_VALUE )
	, _LastInputDTS( AV_NOPTS_VALUE )
	, _LastPacketDuration( 0 )
	, _LastOutputPacketDuration( 0 )
	, _LastWrittenDTS( AV_NOPTS_VALUE )
	, _LastWrittenAudioDTS( AV_NOPTS_VALUE )
	, _AudioInputStreamIndex( -1 )
	, _SegmentStartDTS( 0 )
	, _OutputSegmentStartDTS( AV_NOPTS_VALUE )
	, _CurrentSegmentDuration( 0.0 )
	, _CurrentSegmentNominalDuration( 0.0 )
	, _TimestampProbeSamples( 0 )
	, _TimestampProbeOutliers( 0 )
	, _TimestampProbeInputTicks( 0 )
	, _TimestampProbeDurationTicks( 0 )
	, _SourceTimestampOffset( 0 )
	, _TimestampCorrectionRemainder( 0 )
	, _LastTimestampPhaseError( 0 )
	, _LastTimestampCorrection( 0 )
	, _LastTimestampCorrectionSaturated( false )
	, _CurrentSegmentIndex(0)
	, _CurrentPartialIndex(0)
	, _PartialStartDTS(AV_NOPTS_VALUE)
	, _CurrentPartialDuration(0.0)
	, _CurrentPartialAudioDuration(0.0)
	, _PartialTargetDuration(0.15)
	, _CurrentPartialIsIndependent(false)
	, _CurrentPartialHasPacket(false)
	, _CurrentPartialKeyframeSeekSafe(false)
	, _SegmentTimestampNormalizationActive(false)
	, _PartialBufferOffset(0)
	, _DiscontinuityPending(false)
	, _InitGeneration(0)
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
			av_interleaved_write_frame(_FormatContext, nullptr);
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
	// Tear down FFmpeg state but keep segments/init so HLS.js doesn't lose its reference
	if (_FormatContext)
	{
		// Do NOT call av_write_frame(NULL)/avio_flush here — the muxer may
		// be in an inconsistent state (e.g. audio stream created but no
		// audio packets written), and flushing can cause a crash.
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
	_HasBFrames = false;
	_HasAudioStream = false;
	_NormalizeNoBFrameTimestamps = false;
	_TimestampNormalizationRejected = false;
	_InitialDTS = 0;
	_InitialTimestampUs = AV_NOPTS_VALUE;
	_LastInputDTS = AV_NOPTS_VALUE;
	_LastPacketDuration = 0;
	_LastOutputPacketDuration = 0;
	_LastWrittenDTS = AV_NOPTS_VALUE;
	_LastWrittenAudioDTS = AV_NOPTS_VALUE;
	_AudioInputStreamIndex = -1;
	_OutputSegmentStartDTS = AV_NOPTS_VALUE;
	_TimestampProbeSamples = 0;
	_TimestampProbeOutliers = 0;
	_TimestampProbeInputTicks = 0;
	_TimestampProbeDurationTicks = 0;
	_SourceTimestampOffset = 0;
	_TimestampCorrectionRemainder = 0;
	_LastTimestampPhaseError = 0;
	_LastTimestampCorrection = 0;
	_LastTimestampCorrectionSaturated = false;
	_PartialBufferOffset = 0;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialAudioDuration = 0.0;
	_CurrentSegmentNominalDuration = 0.0;
	_CurrentPartialIsIndependent = false;
	_CurrentPartialHasPacket = false;
	_CurrentPartialKeyframeSeekSafe = false;
	_DiscontinuityPending = true;
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_InitGeneration++;
		_DiagVideoPhaseErrorMs = 0.0;
		_DiagVideoCorrectionMs = 0.0;
		_DiagLastVideoOutputUs = 0;
		_DiagLastAudioOutputUs = 0;
		_DiagHasVideoOutputTimestamp = false;
		_DiagHasAudioOutputTimestamp = false;
	}

	// Notify MSE subscribers of discontinuity before new init segment arrives
	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::Discontinuity;
		Event.Generation = _InitGeneration;
		_EventCallback(Event);
	}

	LOG_INFO("[HLS] Live stream reconnect (generation %d), segments so far: %d, cumulative drift: %.1fms",
		_InitGeneration, _DiagTotalSegments,
		(_DiagTotalAccumulatedDuration - _DiagTotalDtsDuration) * 1000.0);

	// Remove any orphaned incomplete segment from the backlog —
	// otherwise HLS.js will try to load it and get a 404.
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

	// Detect B-frame usage for conditional PTS=DTS handling
	_HasBFrames = InID.CodecContext->has_b_frames > 0;
	LOG_INFO("[HLS] Camera stream %s B-frames (has_b_frames=%d)",
		_HasBFrames ? "has" : "does not have", InID.CodecContext->has_b_frames);

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

	if (InID.HasAudio)
	{
		AVStream* AudioInStream = InID.FormatContext->streams[InID.ChosenAudioStreamIndex];
		AVStream* AudioOutStream = avformat_new_stream(_FormatContext, nullptr);
		if (!AudioOutStream || avcodec_parameters_copy(AudioOutStream->codecpar, AudioInStream->codecpar) < 0)
		{
			STREAM_ERROR(DecoderReceiverError, 0);
		}
		AudioOutStream->codecpar->codec_tag = 0;
		AudioOutStream->time_base = AudioInStream->time_base;
		_HasAudioStream = true;
		_AudioInputStreamIndex = InID.ChosenAudioStreamIndex;
		LOG_INFO("[HLS] AAC audio passthrough enabled");
	}

	// Set up in-memory I/O
	SetupMemoryIO();

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	if (!_FormatContext)
	{
		CameraStreamError Result = InitFormatContext();
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}
	const bool IsAudio = _HasAudioStream && Packet->stream_index == _AudioInputStreamIndex;
	const bool IsVideo = Packet->stream_index == _InputStream->GetData().ChosenStreamIndex;
	if (!IsAudio && !IsVideo)
		return CameraStreamError::Success;
	const bool IsVideoKeyframe = IsVideo && (Packet->flags & AV_PKT_FLAG_KEY);
	// Never mutate segment state around a keyframe that cannot be placed on the
	// decode timeline. Continue waiting for the next usable random-access point.
	if (IsVideo && Packet->dts == AV_NOPTS_VALUE)
	{
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagMissingVideoDtsPackets;
			++_DiagDroppedVideoPackets;
		}
		++_SegmentMissingVideoDtsPackets;
		++_SegmentDroppedVideoPackets;
		return CameraStreamError::Success;
	}

	// Until the first usable random-access frame, ignore both tracks. This keeps
	// the initial timestamp anchored to video and preserves the old join behavior.
	if (!_HeaderWritten && !IsVideoKeyframe)
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

	if (PacketCopy.dts == AV_NOPTS_VALUE)
	{
		// AAC from some RTSP cameras carries only PTS. A missing timestamp on
		// one optional audio packet must not tear down the whole camera session.
		if (!IsAudio || PacketCopy.pts == AV_NOPTS_VALUE)
		{
			av_packet_unref(&PacketCopy);
			return CameraStreamError::Success;
		}
		PacketCopy.dts = PacketCopy.pts;
	}
	if (PacketCopy.pts == AV_NOPTS_VALUE)
		PacketCopy.pts = PacketCopy.dts;

	AVRational InputTimebase = _InputStream->GetData().FormatContext->streams[Packet->stream_index]->time_base;
	int64_t PacketTimestampUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
	if (_InitialTimestampUs == AV_NOPTS_VALUE && IsVideo)
		_InitialTimestampUs = PacketTimestampUs;
	if (_InitialTimestampUs == AV_NOPTS_VALUE || PacketTimestampUs < _InitialTimestampUs)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagDroppedVideoPackets;
			++_SegmentDroppedVideoPackets;
		}
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	PacketCopy.dts = av_rescale_q(PacketTimestampUs - _InitialTimestampUs, AV_TIME_BASE_Q, InputTimebase);
	PacketCopy.pts = av_rescale_q(
		av_rescale_q(PacketCopy.pts, InputTimebase, AV_TIME_BASE_Q) - _InitialTimestampUs,
		AV_TIME_BASE_Q, InputTimebase);

	// Guard against negative timestamps from B-frame reordering at stream start
	if (PacketCopy.dts != AV_NOPTS_VALUE && PacketCopy.dts < 0)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagDroppedVideoPackets;
			++_SegmentDroppedVideoPackets;
		}
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}

	// Clamp invalid durations before they participate in timestamp repair.
	if (PacketCopy.duration < 0 || PacketCopy.duration > INT_MAX)
		PacketCopy.duration = 0;

	// Reject duplicate or out-of-order input packets unless this source has
	// already qualified for duration-derived timestamp repair. In that case the
	// packets are distinct access units in arrival order; discarding one can lose
	// an HEVC reference picture and visibly corrupt dependent frames.
	bool RepairedRegressingVideoTimestamp = false;
	// Also sample the relationship between source DTS deltas
	// and declared frame durations. We only replace the source clock when it is
	// demonstrably jittery but agrees with the durations over the whole window;
	// genuine variable-frame-rate streams therefore retain their source timing.
	if (IsVideo && PacketCopy.dts != AV_NOPTS_VALUE)
	{
		if (_LastInputDTS != AV_NOPTS_VALUE && PacketCopy.dts <= _LastInputDTS)
		{
			const bool CanRepairRegression =
				!_HasBFrames && _AllowTimestampNormalization &&
				!_TimestampNormalizationRejected && _NormalizeNoBFrameTimestamps &&
				_LastWrittenDTS != AV_NOPTS_VALUE && _LastPacketDuration > 0;
			if (!CanRepairRegression)
			{
				uint64_t DroppedVideoPackets;
				{
					const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
					DroppedVideoPackets = ++_DiagDroppedVideoPackets;
				}
				++_SegmentDroppedVideoPackets;
				if (DroppedVideoPackets <= 5 || (DroppedVideoPackets % 100) == 0)
				{
					LOG_WARNING(
						"[HLS] Source %d dropped non-monotonic video packet: previous DTS=%lld, DTS=%lld, PTS=%lld, duration=%lld, flags=0x%x, size=%d, normalizing=%d",
						_InputStream->GetSourceId(), (long long)_LastInputDTS,
						(long long)PacketCopy.dts, (long long)PacketCopy.pts,
						(long long)PacketCopy.duration, PacketCopy.flags, PacketCopy.size,
						_NormalizeNoBFrameTimestamps ? 1 : 0);
				}
				av_packet_unref(&PacketCopy);
				return CameraStreamError::Success;
			}

			RepairedRegressingVideoTimestamp = true;
			uint64_t RepairedVideoTimestamps;
			{
				const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
				RepairedVideoTimestamps = ++_DiagRepairedVideoTimestamps;
			}
			if (RepairedVideoTimestamps <= 5 || (RepairedVideoTimestamps % 100) == 0)
			{
				LOG_WARNING(
					"[HLS] Source %d repaired non-monotonic video timestamp: previous DTS=%lld, DTS=%lld, PTS=%lld, duration=%lld, flags=0x%x, size=%d",
					_InputStream->GetSourceId(), (long long)_LastInputDTS,
					(long long)PacketCopy.dts, (long long)PacketCopy.pts,
					(long long)PacketCopy.duration, PacketCopy.flags, PacketCopy.size);
			}
		}

		if (!_HasBFrames && _AllowTimestampNormalization && !_TimestampNormalizationRejected &&
			_LastInputDTS != AV_NOPTS_VALUE && _LastPacketDuration > 0)
		{
			const int64_t InputDelta = PacketCopy.dts - _LastInputDTS;
			int64_t DeltaError = InputDelta - _LastPacketDuration;
			if (DeltaError < 0)
				DeltaError = -DeltaError;

			++_TimestampProbeSamples;
			_TimestampProbeInputTicks += InputDelta;
			_TimestampProbeDurationTicks += _LastPacketDuration;
			if (DeltaError * 2 > _LastPacketDuration)
				++_TimestampProbeOutliers;
			if (!_NormalizeNoBFrameTimestamps && _TimestampProbeSamples >= 20)
			{
				int64_t TotalError = _TimestampProbeInputTicks - _TimestampProbeDurationTicks;
				if (TotalError < 0)
					TotalError = -TotalError;

				const bool AverageCadenceMatches =
					TotalError * 100 <= _TimestampProbeDurationTicks * 15;
				const bool SourceClockIsJittery =
					_TimestampProbeOutliers * 5 >= _TimestampProbeSamples;
				if (AverageCadenceMatches && SourceClockIsJittery)
				{
					_NormalizeNoBFrameTimestamps = true;
					_SourceTimestampOffset = 0;
					LOG_INFO("[HLS] Normalizing jittery no-B-frame timestamps (%d/%d outliers)",
						_TimestampProbeOutliers, _TimestampProbeSamples);
					_TimestampProbeSamples = 0;
					_TimestampProbeOutliers = 0;
					_TimestampProbeInputTicks = 0;
					_TimestampProbeDurationTicks = 0;
				}
				else if (_TimestampProbeSamples >= 120)
				{
					_TimestampProbeSamples = 0;
					_TimestampProbeOutliers = 0;
					_TimestampProbeInputTicks = 0;
					_TimestampProbeDurationTicks = 0;
				}
			}
			else if (_NormalizeNoBFrameTimestamps)
			{
				// Reolink DTS can wander by several seconds and then converge again.
				// Judge cadence over a long interval rather than using a fixed
				// phase-error budget, which incorrectly rejected camera 11.
				const int64_t ProbeDurationUs = av_rescale_q(
					_TimestampProbeDurationTicks, InputTimebase, AV_TIME_BASE_Q);
				if (ProbeDurationUs >= 120 * AV_TIME_BASE)
				{
					const int64_t SignedError =
						_TimestampProbeInputTicks - _TimestampProbeDurationTicks;
					const int64_t AbsoluteError = SignedError < 0 ? -SignedError : SignedError;
					const bool WindowCadenceDrifted =
						AbsoluteError * 100 > _TimestampProbeDurationTicks * 5;
					const double DriftMs =
						(double)SignedError * InputTimebase.num * 1000.0 / InputTimebase.den;

					if (WindowCadenceDrifted)
					{
						_NormalizeNoBFrameTimestamps = false;
						_TimestampNormalizationRejected = true;
						_SourceTimestampOffset =
							(_LastWrittenDTS + _LastOutputPacketDuration) - PacketCopy.dts;
						LOG_WARNING(
							"[HLS] Source %d stopped timestamp normalization: %.1fms drift over %.1fs (%d samples)",
							_InputStream->GetSourceId(), DriftMs, ProbeDurationUs / 1000000.0,
							_TimestampProbeSamples);
					}

					_TimestampProbeSamples = 0;
					_TimestampProbeOutliers = 0;
					_TimestampProbeInputTicks = 0;
					_TimestampProbeDurationTicks = 0;
				}
			}
		}

		_LastInputDTS = PacketCopy.dts;
	}

	// Segment state must only change after the keyframe has passed timestamp
	// acceptance. Otherwise a rejected keyframe can advertise dependent video as
	// an independent fragment.
	if (IsVideoKeyframe)
	{
		// Enforce a minimum segment duration of 1 second. Cameras like Tapo send
		// keyframes every ~50-100ms, which would create unusable micro-segments.
		const bool ShouldSplit = !_HeaderWritten || _CurrentSegmentDuration >= 1.0;
		if (ShouldSplit)
		{
			if (_HeaderWritten)
				FinishCurrentSegment(PacketCopy.dts);

			CameraStreamError StartResult = StartNewSegment(&PacketCopy);
			if (StartResult != CameraStreamError::Success)
			{
				av_packet_unref(&PacketCopy);
				return StartResult;
			}
		}
	}

	if (!_HeaderWritten || !_CurrentBuffer)
	{
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	if (RepairedRegressingVideoTimestamp)
		++_SegmentRepairedVideoTimestamps;

	// A missing duration must not switch a normalized stream back to its raw
	// timestamp domain for one packet. Reuse the last validated duration.
	if (IsVideo && _NormalizeNoBFrameTimestamps && PacketCopy.duration == 0 && _LastPacketDuration > 0)
		PacketCopy.duration = _LastPacketDuration;
	const int64_t NominalVideoDuration = IsVideo ? PacketCopy.duration : 0;

	// Once the Reolink source has met the guarded jitter test, build a continuous
	// output clock from declared durations, then gently steer it towards the raw
	// source clock shared with audio. A duration-only clock runs at a measurably
	// different rate on some cameras, eventually placing AAC seconds ahead of
	// video. The ten-second response window rejects short DTS wander while the 5%
	// limit keeps every output interval positive and close to the nominal cadence.
	// Otherwise preserve source deltas, applying an offset only when transitioning
	// out of normalized mode.
	if (IsVideo && !_HasBFrames)
	{
		_LastTimestampPhaseError = 0;
		_LastTimestampCorrection = 0;
		_LastTimestampCorrectionSaturated = false;
		if (_NormalizeNoBFrameTimestamps && _LastWrittenDTS != AV_NOPTS_VALUE &&
			_LastPacketDuration > 0 && _LastOutputPacketDuration > 0)
		{
			const int64_t RawDTS = PacketCopy.dts;
			// The previous packet advertised this duration, so using it here keeps
			// FFmpeg's fragment boundary rewrite exactly on the same timeline.
			const int64_t ExpectedDTS = _LastWrittenDTS + _LastOutputPacketDuration;
			const int64_t TargetDTS = RawDTS + _SourceTimestampOffset;
			const int64_t PhaseError = TargetDTS - ExpectedDTS;
			const int64_t ResponseWindowTicks = av_rescale_q(
				10 * AV_TIME_BASE, AV_TIME_BASE_Q, InputTimebase);
			const int64_t MaxCorrection = (std::min)(
				NominalVideoDuration / 20, (int64_t)INT_MAX - NominalVideoDuration);
			int64_t Correction = 0;
			if (ResponseWindowTicks > 0)
			{
				// Anything beyond one second of phase error is already far into the
				// 5% clamp. Bound before multiplying to avoid signed overflow on fine
				// timebases or corrupt source timestamps.
				const int64_t PhaseLimit = (std::max<int64_t>)(1, ResponseWindowTicks / 10);
				const int64_t BoundedPhaseError = (std::max)(-PhaseLimit,
					(std::min)(PhaseError, PhaseLimit));
				const int64_t Numerator =
					BoundedPhaseError * NominalVideoDuration + _TimestampCorrectionRemainder;
				Correction = Numerator / ResponseWindowTicks;
				_TimestampCorrectionRemainder = Numerator % ResponseWindowTicks;
			}
			const int64_t UnclampedCorrection = Correction;
			Correction = (std::max)(-MaxCorrection, (std::min)(Correction, MaxCorrection));
			PacketCopy.dts = ExpectedDTS;
			PacketCopy.duration = NominalVideoDuration + Correction;
			_LastTimestampPhaseError = PhaseError;
			_LastTimestampCorrection = Correction;
			_LastTimestampCorrectionSaturated = Correction != UnclampedCorrection ||
				PhaseError > ResponseWindowTicks / 10 || PhaseError < -ResponseWindowTicks / 10;
		}
		else if (_SourceTimestampOffset != 0 && PacketCopy.dts != AV_NOPTS_VALUE)
			PacketCopy.dts += _SourceTimestampOffset;
		PacketCopy.pts = PacketCopy.dts;
	}

	PacketCopy.stream_index = IsAudio ? 1 : 0;
	PacketCopy.pos = -1;

	// B-frame streams retain their source timing, so keep an output-side
	// monotonicity check as a final muxer safety net.
	const int64_t LastStreamDTS = IsAudio ? _LastWrittenAudioDTS : _LastWrittenDTS;
	if (LastStreamDTS != AV_NOPTS_VALUE && PacketCopy.dts <= LastStreamDTS)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagDroppedVideoPackets;
			++_SegmentDroppedVideoPackets;
		}
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	if (IsVideo)
	{
		_LastWrittenDTS = PacketCopy.dts;
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagAcceptedVideoPackets;
			if (IsVideoKeyframe)
				++_DiagAcceptedVideoKeyframes;
			if (PacketCopy.flags & AV_PKT_FLAG_CORRUPT)
				++_DiagCorruptVideoPackets;
			_DiagVideoPhaseErrorMs = (double)_LastTimestampPhaseError *
				InputTimebase.num * 1000.0 / InputTimebase.den;
			_DiagVideoCorrectionMs = (double)_LastTimestampCorrection *
				InputTimebase.num * 1000.0 / InputTimebase.den;
			_DiagLastVideoOutputUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
			_DiagHasVideoOutputTimestamp = true;
			if (_LastTimestampCorrectionSaturated)
				++_DiagTimestampCorrectionSaturatedPackets;
		}
		++_SegmentAcceptedVideoPackets;
		if (IsVideoKeyframe)
		{
			++_SegmentAcceptedVideoKeyframes;
		}
		if (PacketCopy.flags & AV_PKT_FLAG_CORRUPT)
		{
			++_SegmentCorruptVideoPackets;
		}
		if (_NormalizeNoBFrameTimestamps)
			_SegmentTimestampNormalizationActive = true;
		if (NominalVideoDuration > 0)
			_LastPacketDuration = NominalVideoDuration;
		if (PacketCopy.duration > 0)
			_LastOutputPacketDuration = PacketCopy.duration;
	}
	else
	{
		_LastWrittenAudioDTS = PacketCopy.dts;
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_DiagLastAudioOutputUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
		_DiagHasAudioOutputTimestamp = true;
	}

	if (IsVideo && _OutputSegmentStartDTS == AV_NOPTS_VALUE)
		_OutputSegmentStartDTS = PacketCopy.dts;

	if (IsVideo)
	{
		AVRational TimeBase = _FormatContext->streams[0]->time_base;
		double PacketDurationSec = (double)(PacketCopy.duration * TimeBase.num) / TimeBase.den;
		double NominalPacketDurationSec =
			(double)(NominalVideoDuration * TimeBase.num) / TimeBase.den;
		_CurrentSegmentDuration += PacketDurationSec;
		_CurrentSegmentNominalDuration += NominalPacketDurationSec;
		_CurrentPartialDuration += PacketDurationSec;

		// With multiplexed audio, SourceBuffer range boundaries are not guaranteed
		// to identify the exact video RAP timestamp.
		const bool PacketKeyframeSeekSafe =
			!_HasAudioStream && !_HasBFrames && _NormalizeNoBFrameTimestamps;
		if (!_CurrentPartialHasPacket)
		{
			_CurrentPartialHasPacket = true;
			_CurrentPartialKeyframeSeekSafe = PacketKeyframeSeekSafe;
		}
		else
		{
			_CurrentPartialKeyframeSeekSafe =
				_CurrentPartialKeyframeSeekSafe && PacketKeyframeSeekSafe;
		}
	}
	else if (IsAudio)
	{
		double PacketDurationSec = (double)(PacketCopy.duration * InputTimebase.num) / InputTimebase.den;
		_CurrentPartialAudioDuration += PacketDurationSec;
	}

	// Track whether this partial contains a keyframe (first partial of segment)
	if (IsVideo && (PacketCopy.flags & AV_PKT_FLAG_KEY))
		_CurrentPartialIsIndependent = true;

	av_packet_rescale_ts(&PacketCopy, InputTimebase,
		_FormatContext->streams[PacketCopy.stream_index]->time_base);
	// Multi-track fMP4 needs FFmpeg's interleaver to emit a valid fragment.
	Result = av_interleaved_write_frame(_FormatContext, &PacketCopy);
	av_packet_unref(&PacketCopy);
	if (Result < 0)
	{
		STREAM_ERROR(WriteFailed, Result);
	}

	// Flush a partial segment when we've accumulated enough duration
	if (IsVideo && _CurrentPartialDuration >= _PartialTargetDuration)
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
	av_interleaved_write_frame(_FormatContext, nullptr);
	av_write_frame(_FormatContext, nullptr);
	avio_flush(_FormatContext->pb);

	// Only create a partial if we actually accumulated data since the last flush
	size_t CurrentSize = _CurrentBuffer->size();
	// windows.h defines max as a macro in this translation unit.
	double PartialDuration = (std::max)(_CurrentPartialDuration, _CurrentPartialAudioDuration);
	if (CurrentSize <= _PartialBufferOffset)
		return;
	// Some RTSP sources omit AAC packet durations. The fragment still needs to
	// be delivered; use the configured target as a conservative playlist value.
	if (PartialDuration <= 0.0)
		PartialDuration = _PartialTargetDuration;

	// Create a partial that references the byte range [_PartialBufferOffset, CurrentSize)
	// within the single segment buffer
	auto PartialData = std::make_shared<std::vector<uint8_t>>(
		_CurrentBuffer->begin() + _PartialBufferOffset,
		_CurrentBuffer->begin() + CurrentSize
	);

	_PartialBufferOffset = CurrentSize;

	LiveStreamPartialSegment Partial;
	Partial.Data = PartialData;
	Partial.Duration = PartialDuration;
	Partial.PartIndex = _CurrentPartialIndex;
	Partial.Independent = IsIndependent;
	const bool KeyframeSeekSafe = _CurrentPartialKeyframeSeekSafe;

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		if (!_StreamBacklog->empty())
		{
			_StreamBacklog->back().Partials.push_back(Partial);
		}
	}

	_CurrentPartialIndex++;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialAudioDuration = 0.0;
	_CurrentPartialIsIndependent = false;
	_CurrentPartialHasPacket = false;
	_CurrentPartialKeyframeSeekSafe = false;

	// Notify MSE subscribers of new partial
	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::PartialReady;
		Event.SegmentIndex = _CurrentSegmentIndex;
		Event.PartIndex = Partial.PartIndex;
		Event.Data = Partial.Data;
		Event.Duration = Partial.Duration;
		Event.Independent = Partial.Independent;
		Event.KeyframeSeekSafe = KeyframeSeekSafe;
		Event.Generation = _InitGeneration;
		_EventCallback(Event);
	}
}

CameraStreamError LiveOutputStream::StartNewSegment(const AVPacket* Packet)
{
	if (!_InitSegmentCaptured)
	{
		// Write the init segment to an in-memory buffer
		_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

		AVDictionary* options = nullptr;
		av_dict_set(&options, "movflags", "empty_moov+frag_custom+dash+default_base_moof", 0);
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
		LiveStreamEvent InitEvent;
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			_InitSegmentData = _CurrentBuffer;
			_InitSegmentGeneration = _InitGeneration;
			_InitAudioCodec = _HasAudioStream ? "mp4a.40.2" : "";
			InitEvent.EventType = LiveStreamEvent::InitSegmentReady;
			InitEvent.Data = _InitSegmentData;
			InitEvent.Generation = _InitGeneration;
			InitEvent.AudioCodec = _InitAudioCodec;
		}

		_InitSegmentCaptured = true;

		// Notify MSE subscribers of init segment
		if (_EventCallback)
		{
			_EventCallback(InitEvent);
		}
	}

	// Start a fresh buffer for this segment
	_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

	_SegmentStartDTS = Packet->dts;
	_OutputSegmentStartDTS = AV_NOPTS_VALUE;
	_CurrentSegmentDuration = 0.0;
	_CurrentSegmentNominalDuration = 0.0;
	_CurrentPartialIndex = 0;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialAudioDuration = 0.0;
	_CurrentPartialIsIndependent = true; // first partial starts with keyframe
	_CurrentPartialHasPacket = false;
	_CurrentPartialKeyframeSeekSafe = false;
	_SegmentTimestampNormalizationActive = _NormalizeNoBFrameTimestamps;
	_PartialBufferOffset = 0;
	_SegmentAcceptedVideoPackets = 0;
	_SegmentAcceptedVideoKeyframes = 0;
	_SegmentRepairedVideoTimestamps = 0;
	_SegmentDroppedVideoPackets = 0;
	_SegmentMissingVideoDtsPackets = 0;
	_SegmentCorruptVideoPackets = 0;
	_CurrentSegmentWallTime = std::chrono::system_clock::now();

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		// Trim old segments
		while (_StreamBacklog->size() >= 15)
		{
			_StreamBacklog->erase(_StreamBacklog->begin());
		}

		LiveStreamSegment NewSegment;
		NewSegment.Data = nullptr; // Will be set when finished
		NewSegment.Duration = 0.0;
		NewSegment.SegmentIndex = _CurrentSegmentIndex;
		NewSegment.SegmentTime = _CurrentSegmentWallTime;
		NewSegment.Ready = false;
		NewSegment.Discontinuity = _DiscontinuityPending;
		_DiscontinuityPending = false;

		_StreamBacklog->push_back(NewSegment);
	}

	return CameraStreamError::Success;
}

void LiveOutputStream::FinishCurrentSegment(int64_t NextKeyframeDTS)
{
	if (!_FormatContext || !_CurrentBuffer)
	{
		return;
	}

	// Flush remaining data as the final partial of this segment
	FlushPartialSegment(_CurrentPartialIsIndependent);

	// Compute accurate segment duration from DTS span instead of
	// accumulated packet durations. Accumulated durations drift over
	// thousands of segments because AVPacket.duration values don't
	// exactly match the DTS delta between keyframes.
	AVRational TimeBase = _FormatContext->streams[0]->time_base;
	double DtsDuration = (double)(NextKeyframeDTS - _SegmentStartDTS) * TimeBase.num / TimeBase.den;
	double OutputDuration = _CurrentSegmentDuration;
	if (_OutputSegmentStartDTS != AV_NOPTS_VALUE && _LastWrittenDTS != AV_NOPTS_VALUE && _LastOutputPacketDuration > 0)
	{
		OutputDuration = (double)(_LastWrittenDTS + _LastOutputPacketDuration - _OutputSegmentStartDTS) *
			TimeBase.num / TimeBase.den;
	}

	// Sanity: if DTS duration is clearly wrong, fall back to accumulated
	if (DtsDuration <= 0.0 || DtsDuration > 30.0)
		DtsDuration = _CurrentSegmentNominalDuration;

	// Record diagnostics
	double DriftMs = (_CurrentSegmentNominalDuration - DtsDuration) * 1000.0;
	SegmentDiagEntry Entry;
	Entry.SegmentIndex = _CurrentSegmentIndex;
	Entry.DtsDuration = DtsDuration;
	Entry.OutputDuration = OutputDuration;
	Entry.AccumulatedDuration = _CurrentSegmentNominalDuration;
	Entry.DriftMs = DriftMs;
	Entry.TimestampNormalizationActive = _SegmentTimestampNormalizationActive;
	Entry.AcceptedVideoPackets = _SegmentAcceptedVideoPackets;
	Entry.AcceptedVideoKeyframes = _SegmentAcceptedVideoKeyframes;
	Entry.RepairedVideoTimestamps = _SegmentRepairedVideoTimestamps;
	Entry.DroppedVideoPackets = _SegmentDroppedVideoPackets;
	Entry.MissingVideoDtsPackets = _SegmentMissingVideoDtsPackets;
	Entry.CorruptVideoPackets = _SegmentCorruptVideoPackets;
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_DiagRing[_DiagRingPos % DIAG_RING_SIZE] = Entry;
		_DiagRingPos++;
		if (_DiagRingCount < DIAG_RING_SIZE) _DiagRingCount++;
		_DiagTotalSegments++;
		_DiagTotalDtsDuration += DtsDuration;
		_DiagTotalAccumulatedDuration += _CurrentSegmentNominalDuration;
		if (std::abs(DriftMs) > std::abs(_DiagMaxDriftMs))
			_DiagMaxDriftMs = DriftMs;

		if (!_StreamBacklog->empty())
		{
			auto& Seg = _StreamBacklog->back();
			Seg.Data = _CurrentBuffer;
			// Use accumulated packet duration for EXTINF — this matches the
			// actual sample durations in the fMP4 container. Using DTS span
			// causes playlist/media divergence: DTS may differ from packet
			// duration sums by ~0.002s/segment, which over thousands of
			// segments makes the playhead outrun buffered media.
			Seg.Duration = _CurrentSegmentDuration;
			Seg.Ready = true;
		}
		_CurrentSegmentIndex++;
	}

	_CurrentBuffer.reset();

	// Notify MSE subscribers that segment is complete
	// (MSE clients primarily use partials for low latency, but this
	// signals segment boundaries for buffer management)
	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::SegmentReady;
		Event.SegmentIndex = _CurrentSegmentIndex - 1;
		Event.Duration = _CurrentSegmentDuration;
		Event.Generation = _InitGeneration;
		_EventCallback(Event);
	}
}

LiveOutputStream::StreamingDiagnostics LiveOutputStream::GetStreamingDiagnostics() const
{
	StreamingDiagnostics Diag;
	const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
	Diag.TotalSegments = _DiagTotalSegments;
	Diag.ReconnectCount = _InitGeneration;
	Diag.TotalDtsDuration = _DiagTotalDtsDuration;
	Diag.TotalAccumulatedDuration = _DiagTotalAccumulatedDuration;
	Diag.MaxDriftMs = _DiagMaxDriftMs;
	Diag.CurrentSegmentIndex = _CurrentSegmentIndex;
	Diag.InitGeneration = _InitGeneration;
	Diag.AcceptedVideoPackets = _DiagAcceptedVideoPackets;
	Diag.AcceptedVideoKeyframes = _DiagAcceptedVideoKeyframes;
	Diag.RepairedVideoTimestamps = _DiagRepairedVideoTimestamps;
	Diag.DroppedVideoPackets = _DiagDroppedVideoPackets;
	Diag.MissingVideoDtsPackets = _DiagMissingVideoDtsPackets;
	Diag.CorruptVideoPackets = _DiagCorruptVideoPackets;
	Diag.VideoPhaseErrorMs = _DiagVideoPhaseErrorMs;
	Diag.VideoCorrectionMs = _DiagVideoCorrectionMs;
	if (_DiagHasAudioOutputTimestamp && _DiagHasVideoOutputTimestamp)
		Diag.AudioVideoSkewMs = (_DiagLastAudioOutputUs - _DiagLastVideoOutputUs) / 1000.0;
	Diag.TimestampCorrectionSaturatedPackets = _DiagTimestampCorrectionSaturatedPackets;
	Diag.BacklogSize = (int)_StreamBacklog->size();

	// Copy recent segment entries from ring buffer
	int count = _DiagRingCount;
	if (count > 0)
		Diag.TimestampNormalizationActive = _DiagRing[(_DiagRingPos - 1) % DIAG_RING_SIZE].TimestampNormalizationActive;
	int start = (_DiagRingPos - count);
	if (start < 0) start += DIAG_RING_SIZE;
	for (int i = 0; i < count; i++)
	{
		Diag.RecentSegments.push_back(_DiagRing[(start + i) % DIAG_RING_SIZE]);
	}

	return Diag;
}

}}
