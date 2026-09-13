#pragma once

#include "Stream.h"
#include "InputStream.h"
#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <functional>

struct AVPacket;
struct AVRational;
struct AVFormatContext;
struct AVCodecContext;
struct AVIOContext;

namespace Witness{
namespace Camera{

typedef std::shared_ptr<std::vector<uint8_t>> SegmentBuffer;

struct LiveStreamInitSnapshot
{
	SegmentBuffer Data;
	int Generation = 0;
	std::string AudioCodec;
};

// Notification events emitted by LiveOutputStream for MSE WebSocket streaming
struct CAMERA_API LiveStreamEvent
{
	enum Type
	{
		InitSegmentReady,    // Init segment (ftyp+moov) captured or recaptured
		PartialReady,        // A partial segment (~0.33s) has been flushed
		SegmentReady,        // A full segment is complete and ready
		Discontinuity        // Camera reconnected — new init segment coming
	};

	Type EventType;
	int SegmentIndex = 0;
	int PartIndex = 0;
	SegmentBuffer Data;       // Binary fMP4 data (init, partial, or full segment)
	double Duration = 0.0;
	bool Independent = false; // True if partial starts with keyframe
	bool KeyframeSeekSafe = false; // Sequence boundary is an exact no-B-frame RAP timestamp
	int Generation = 0;       // Init segment generation counter
	std::string AudioCodec;    // MSE codec string when the init has an audio track
};

using LiveStreamEventCallback = std::function<void(const LiveStreamEvent&)>;

struct LiveStreamPartialSegment
{
	SegmentBuffer Data;
	double Duration;
	int PartIndex;
	bool Independent; // true if starts with a keyframe
};

struct LiveStreamSegment
{
	SegmentBuffer Data;
	double Duration;
	int SegmentIndex;
	std::chrono::time_point<std::chrono::system_clock> SegmentTime;
	bool Ready;
	bool Discontinuity; // true if this segment follows a camera reconnect
	std::vector<LiveStreamPartialSegment> Partials;
};

class CAMERA_API LiveOutputStream : public Stream
{
public:
	LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment);
	virtual ~LiveOutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet );

	int GetCurrentSegment()
	{
		return _CurrentSegmentIndex;
	}

	void GetSegments(std::vector<LiveStreamSegment>& OutSegments )
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		// Prune segments older than 10s so clients don't loop stale data.
		auto now = std::chrono::system_clock::now();
		std::erase_if( *_StreamBacklog, [&now]( const LiveStreamSegment& seg )
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				now - seg.SegmentTime ).count() > 10;
		});

		OutSegments = *_StreamBacklog;
	}

	SegmentBuffer GetInitSegment()
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		return _InitSegmentData;
	}

	int GetInitGeneration()
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		return _InitGeneration;
	}

	LiveStreamInitSnapshot GetInitSnapshot()
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		return { _InitSegmentData, _InitSegmentGeneration, _InitAudioCodec };
	}

	double GetPartialTargetDuration()
	{
		return _PartialTargetDuration;
	}

	void SetPartialTargetDuration(double Duration)
	{
		_PartialTargetDuration = Duration;
	}

	// Timestamp repair is deliberately opt-in for camera profiles known to
	// emit jittery RTSP clocks; generic and genuine VFR streams are untouched.
	void SetTimestampNormalizationAllowed(bool Allowed)
	{
		_AllowTimestampNormalization = Allowed;
	}

	void ResetForReconnect(InputStream* NewInputStream);

	// Observer for MSE WebSocket streaming — called on camera worker thread
	void SetEventCallback(LiveStreamEventCallback Callback)
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_EventCallback = std::move(Callback);
	}

private:

	CameraStreamError InitFormatContext();
	CameraStreamError StartNewSegment(const AVPacket* Packet);
	void FinishCurrentSegment(int64_t NextKeyframeDTS);
	void FlushPartialSegment(bool IsIndependent);

	void SetupMemoryIO();

	static int WriteBuffer(void* Opaque, const uint8_t* Buffer, int BufferSize);
	static int64_t SeekBuffer(void* Opaque, int64_t Offset, int Origin);

	std::string _LiveCachePath;

	std::vector<LiveStreamSegment>* _StreamBacklog;

	InputStream* _InputStream;

	// Single persistent format context for the entire live stream
	AVFormatContext* _FormatContext;
	AVIOContext* _AVIOContext;
	uint8_t* _AVIOBuffer;

	// Current write target — FFmpeg writes here via callbacks
	SegmentBuffer _CurrentBuffer;

	// Init segment stored in memory
	SegmentBuffer _InitSegmentData;
	int _InitSegmentGeneration = 0;
	std::string _InitAudioCodec;

	bool _HeaderWritten;
	bool _InitSegmentCaptured;
	bool _HasInitialDTS;
	bool _HasBFrames;
	bool _HasAudioStream;
	bool _AllowTimestampNormalization;
	bool _NormalizeNoBFrameTimestamps;
	bool _TimestampNormalizationRejected;

	int64_t _InitialDTS;
	int64_t _InitialTimestampUs;
	int64_t _LastInputDTS;
	int64_t _LastPacketDuration;
	int64_t _LastWrittenDTS;
	int64_t _LastWrittenAudioDTS;
	int _AudioInputStreamIndex;
	int64_t _SegmentStartDTS;
	int64_t _OutputSegmentStartDTS;
	double _CurrentSegmentDuration;
	int _TimestampProbeSamples;
	int _TimestampProbeOutliers;
	int64_t _TimestampProbeInputTicks;
	int64_t _TimestampProbeDurationTicks;
	int64_t _SourceTimestampOffset;

	int _CurrentSegmentIndex;

	int _CurrentPartialIndex;
	int64_t _PartialStartDTS;
	double _CurrentPartialDuration;
	double _CurrentPartialAudioDuration;
	double _PartialTargetDuration;
	bool _CurrentPartialIsIndependent;
	bool _CurrentPartialHasPacket;
	bool _CurrentPartialKeyframeSeekSafe;
	size_t _PartialBufferOffset;

	bool _DiscontinuityPending; // set on reconnect, consumed by next segment

	int _InitGeneration; // incremented on reconnect so HLS.js refetches init segment

	std::chrono::time_point<std::chrono::system_clock> _CurrentSegmentWallTime;

	std::mutex* _SegmentsMutex;

	// MSE WebSocket callback — notifies subscribers of new data
	LiveStreamEventCallback _EventCallback;

	// ── Streaming diagnostics ──────────────────────────────────────
public:
	struct SegmentDiagEntry
	{
		int SegmentIndex;
		double DtsDuration;         // raw source DTS span
		double OutputDuration;      // normalized DTS span written to the muxer
		double AccumulatedDuration; // sum of declared packet durations
		double DriftMs;             // (accumulated - source DTS) * 1000
		bool TimestampNormalizationActive;
	};

	struct StreamingDiagnostics
	{
		int TotalSegments = 0;
		int ReconnectCount = 0;
		double TotalDtsDuration = 0.0;
		double TotalAccumulatedDuration = 0.0;
		double MaxDriftMs = 0.0;
		int CurrentSegmentIndex = 0;
		int BacklogSize = 0;
		int InitGeneration = 0;
		bool TimestampNormalizationActive = false;
		std::vector<SegmentDiagEntry> RecentSegments; // last 30
	};

	StreamingDiagnostics GetStreamingDiagnostics() const;

private:
	// Ring buffer of recent segment diagnostics
	static const int DIAG_RING_SIZE = 30;
	SegmentDiagEntry _DiagRing[DIAG_RING_SIZE] = {};
	int _DiagRingPos = 0;
	int _DiagRingCount = 0;
	int _DiagTotalSegments = 0;
	double _DiagTotalDtsDuration = 0.0;
	double _DiagTotalAccumulatedDuration = 0.0;
	double _DiagMaxDriftMs = 0.0;
};

}}
