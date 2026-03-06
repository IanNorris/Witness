#pragma once

#include "Stream.h"
#include "InputStream.h"
#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <memory>

struct AVPacket;
struct AVRational;
struct AVFormatContext;
struct AVCodecContext;
struct AVIOContext;

namespace Witness{
namespace Camera{

typedef std::shared_ptr<std::vector<uint8_t>> SegmentBuffer;

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
		return _InitGeneration;
	}

	double GetPartialTargetDuration()
	{
		return _PartialTargetDuration;
	}

	void ResetForReconnect(InputStream* NewInputStream);

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

	bool _HeaderWritten;
	bool _InitSegmentCaptured;
	bool _HasInitialDTS;

	int64_t _InitialDTS;
	int64_t _LastWrittenDTS;
	int64_t _SegmentStartDTS;
	double _CurrentSegmentDuration;

	int _CurrentSegmentIndex;

	int _CurrentPartialIndex;
	int64_t _PartialStartDTS;
	double _CurrentPartialDuration;
	double _PartialTargetDuration;
	bool _CurrentPartialIsIndependent;
	size_t _PartialBufferOffset;

	bool _DiscontinuityPending; // set on reconnect, consumed by next segment

	int _InitGeneration; // incremented on reconnect so HLS.js refetches init segment

	std::chrono::time_point<std::chrono::system_clock> _CurrentSegmentWallTime;

	std::mutex* _SegmentsMutex;

	// ── Streaming diagnostics ──────────────────────────────────────
public:
	struct SegmentDiagEntry
	{
		int SegmentIndex;
		double DtsDuration;        // computed from DTS span (what goes into EXTINF)
		double AccumulatedDuration; // sum of packet durations (old method)
		double DriftMs;            // (accumulated - dts) * 1000
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
