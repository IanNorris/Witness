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
		Discontinuity,       // Camera reconnected — new init segment coming
		DecodeCorruption,    // Keep decoding but suppress presentation
		DecodeRecovery       // A subsequent random-access frame can be presented
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
	uint64_t ByteSize = 0;     // expected binary WebSocket message size
	uint32_t TransportHash = 0; // FNV-1a hash verified by the browser
	int DecodeErrorFlags = 0;
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
	void NotifyDecodeCorruption(int ErrorFlags);

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
	int64_t _LastOutputPacketDuration;
	int64_t _LastWrittenDTS;
	int64_t _LastWrittenAudioDTS;
	int _AudioInputStreamIndex;
	int64_t _SegmentStartDTS;
	int64_t _OutputSegmentStartDTS;
	double _CurrentSegmentDuration;
	double _CurrentSegmentNominalDuration;
	int _TimestampProbeSamples;
	int _TimestampProbeOutliers;
	int64_t _TimestampProbeInputTicks;
	int64_t _TimestampProbeDurationTicks;
	int64_t _SourceTimestampOffset;
	int64_t _TimestampCorrectionRemainder;
	int64_t _LastTimestampPhaseError;
	int64_t _LastTimestampCorrection;
	bool _LastTimestampCorrectionSaturated;

	int _CurrentSegmentIndex;

	int _CurrentPartialIndex;
	int64_t _PartialStartDTS;
	double _CurrentPartialDuration;
	double _CurrentPartialAudioDuration;
	double _PartialTargetDuration;
	bool _CurrentPartialIsIndependent;
	bool _CurrentPartialHasPacket;
	bool _CurrentPartialKeyframeSeekSafe;
	bool _SegmentTimestampNormalizationActive;
	size_t _PartialBufferOffset;

	bool _DiscontinuityPending; // set on reconnect, consumed by next segment
	bool _DecodeCorruptionActive = false;

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
		uint64_t AcceptedVideoPackets;
		uint64_t AcceptedVideoKeyframes;
		uint64_t RepairedVideoTimestamps;
		uint64_t DroppedVideoPackets;
		uint64_t MissingVideoDtsPackets;
		uint64_t CorruptVideoPackets;
		uint64_t PacketPayloadHash;
		uint64_t FragmentHash;
		uint64_t FragmentBytes;
		bool FragmentStructureValid;
		int FragmentBoxCount;
		int FragmentMoofCount;
		int FragmentMdatCount;
		int FragmentStructureError;
		uint64_t FragmentErrorOffset;
	};

	struct FragmentDiagEntry
	{
		int Generation = 0;
		int SegmentIndex = 0;
		int PartIndex = 0;
		bool Independent = false;
		bool KeyframeSeekSafe = false;
		uint64_t Bytes = 0;
		uint64_t Hash = 0;
		bool StructureValid = false;
		int BoxCount = 0;
		int MoofCount = 0;
		int MdatCount = 0;
		int StructureError = 0;
		uint64_t ErrorOffset = 0;
	};

	struct PacketDiagEntry
	{
		uint64_t Sequence = 0;
		int Generation = 0;
		int SegmentIndex = 0;
		int PartialIndex = 0;
		bool Audio = false;
		bool Keyframe = false;
		bool Corrupt = false;
		bool DtsSynthesized = false;
		bool PtsSynthesized = false;
		bool DurationSynthesized = false;
		bool TimestampNormalized = false;
		bool TimestampRepaired = false;
		bool CorrectionSaturated = false;
		int Size = 0;
		int Flags = 0;
		int64_t SourceDtsUs = 0;
		int64_t SourcePtsUs = 0;
		int64_t SourceDurationUs = 0;
		int64_t OutputDtsUs = 0;
		int64_t OutputPtsUs = 0;
		int64_t OutputDurationUs = 0;
		bool HasSourceDts = false;
		bool HasSourcePts = false;
		bool HasOutputDts = false;
		bool HasOutputPts = false;
		uint64_t PayloadHash = 0;
		int64_t ArrivalMs = 0;
		int Packetization = 0; // 0=opaque, 1=Annex B, 2=length-prefixed, 3=ADTS
		int CodecUnitCount = 0;
		int PrimaryCodecUnitType = -1;
		uint8_t PayloadPrefix[16] = {};
		int PayloadPrefixLength = 0;
		std::string Disposition;
	};

	struct MediaDiagnosticEvent
	{
		uint64_t Sequence = 0;
		uint64_t ActivityID = 0;
		uint64_t PacketSequence = 0;
		int64_t TimestampUnixMs = 0;
		int64_t ElapsedMs = 0;
		int Generation = 0;
		int SegmentIndex = 0;
		int PartialIndex = 0;
		std::string Category;
		std::string Severity;
		std::string Phase;
		std::string Component;
		std::string Message;
		std::string Disposition;
		bool Audio = false;
		bool Keyframe = false;
		bool Corrupt = false;
		int PacketSize = 0;
		int64_t SourceDtsUs = 0;
		int64_t SourcePtsUs = 0;
		bool HasSourceDts = false;
		bool HasSourcePts = false;
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
		uint64_t AcceptedVideoPackets = 0;
		uint64_t AcceptedVideoKeyframes = 0;
		uint64_t RepairedVideoTimestamps = 0;
		uint64_t DroppedVideoPackets = 0;
		uint64_t MissingVideoDtsPackets = 0;
		uint64_t CorruptVideoPackets = 0;
		uint64_t WaitingForKeyframePackets = 0;
		uint64_t MissingTimestampPackets = 0;
		uint64_t BeforeVideoEpochPackets = 0;
		uint64_t NegativeTimestampPackets = 0;
		uint64_t NonMonotonicInputPackets = 0;
		uint64_t NoMuxBufferPackets = 0;
		uint64_t NonMonotonicOutputPackets = 0;
		uint64_t MuxErrorPackets = 0;
		uint64_t DecodeCorruptionEvents = 0;
		uint64_t DecodeRecoveryEvents = 0;
		double VideoPhaseErrorMs = 0.0;
		double VideoCorrectionMs = 0.0;
		double AudioVideoSkewMs = 0.0;
		bool HasAudioVideoSkew = false;
		uint64_t TimestampCorrectionSaturatedPackets = 0;
		std::string VideoCodec;
		std::string AudioCodec;
		std::string InputFormat;
		int VideoProfile = 0;
		int VideoLevel = 0;
		int VideoWidth = 0;
		int VideoHeight = 0;
		int VideoTimeBaseNum = 0;
		int VideoTimeBaseDen = 0;
		int VideoExtradataBytes = 0;
		uint64_t VideoExtradataHash = 0;
		int AudioProfile = 0;
		int AudioSampleRate = 0;
		int AudioChannels = 0;
		int AudioTimeBaseNum = 0;
		int AudioTimeBaseDen = 0;
		int AudioExtradataBytes = 0;
		uint64_t AudioExtradataHash = 0;
		bool InitStructureValid = false;
		bool InitStructureObserved = false;
		int InitBoxCount = 0;
		int InitFtypCount = 0;
		int InitMoovCount = 0;
		int InitStructureError = 0;
		uint64_t InitErrorOffset = 0;
		std::vector<SegmentDiagEntry> RecentSegments; // last 30
		std::vector<FragmentDiagEntry> RecentFragments; // last 180 MSE partials
		std::vector<PacketDiagEntry> RecentPackets; // last 1024 audio/video access units
		std::vector<MediaDiagnosticEvent> RecentMediaEvents; // last 48 warnings/errors/actions
		struct AnomalyCapture
		{
			uint64_t Sequence = 0;
			int64_t CapturedAtMs = 0;
			int Generation = 0;
			int SegmentIndex = 0;
			std::string Reason;
			std::vector<FragmentDiagEntry> Fragments;
			std::vector<PacketDiagEntry> Packets;
		};
		std::vector<AnomalyCapture> Anomalies; // last 3 client-reported failures
	};

	StreamingDiagnostics GetStreamingDiagnostics( bool IncludeHistory = true ) const;
	void CaptureDiagnosticAnomaly(const std::string& Reason);
	uint64_t BeginDiagnosticActivity();
	void RecordFFmpegLog( int Level, const char* Phase, const char* Component,
		const char* Message, uint64_t ActivityID );

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
	uint64_t _DiagAcceptedVideoPackets = 0;
	uint64_t _DiagAcceptedVideoKeyframes = 0;
	uint64_t _DiagRepairedVideoTimestamps = 0;
	uint64_t _DiagDroppedVideoPackets = 0;
	uint64_t _DiagMissingVideoDtsPackets = 0;
	uint64_t _DiagCorruptVideoPackets = 0;
	uint64_t _DiagWaitingForKeyframePackets = 0;
	uint64_t _DiagMissingTimestampPackets = 0;
	uint64_t _DiagBeforeVideoEpochPackets = 0;
	uint64_t _DiagNegativeTimestampPackets = 0;
	uint64_t _DiagNonMonotonicInputPackets = 0;
	uint64_t _DiagNoMuxBufferPackets = 0;
	uint64_t _DiagNonMonotonicOutputPackets = 0;
	uint64_t _DiagMuxErrorPackets = 0;
	uint64_t _DiagDecodeCorruptionEvents = 0;
	uint64_t _DiagDecodeRecoveryEvents = 0;
	double _DiagVideoPhaseErrorMs = 0.0;
	double _DiagVideoCorrectionMs = 0.0;
	int64_t _DiagLastVideoOutputUs = 0;
	int64_t _DiagLastAudioOutputUs = 0;
	bool _DiagHasVideoOutputTimestamp = false;
	bool _DiagHasAudioOutputTimestamp = false;
	uint64_t _DiagTimestampCorrectionSaturatedPackets = 0;
	uint64_t _SegmentAcceptedVideoPackets = 0;
	uint64_t _SegmentAcceptedVideoKeyframes = 0;
	uint64_t _SegmentRepairedVideoTimestamps = 0;
	uint64_t _SegmentDroppedVideoPackets = 0;
	uint64_t _SegmentMissingVideoDtsPackets = 0;
	uint64_t _SegmentCorruptVideoPackets = 0;
	uint64_t _SegmentPacketPayloadHash = 14695981039346656037ULL;
	std::string _DiagVideoCodec;
	std::string _DiagAudioCodec;
	std::string _DiagInputFormat;
	int _DiagVideoProfile = 0;
	int _DiagVideoLevel = 0;
	int _DiagVideoWidth = 0;
	int _DiagVideoHeight = 0;
	int _DiagVideoTimeBaseNum = 0;
	int _DiagVideoTimeBaseDen = 0;
	int _DiagVideoExtradataBytes = 0;
	uint64_t _DiagVideoExtradataHash = 0;
	int _DiagAudioProfile = 0;
	int _DiagAudioSampleRate = 0;
	int _DiagAudioChannels = 0;
	int _DiagAudioTimeBaseNum = 0;
	int _DiagAudioTimeBaseDen = 0;
	int _DiagAudioExtradataBytes = 0;
	uint64_t _DiagAudioExtradataHash = 0;
	bool _DiagInitStructureValid = false;
	bool _DiagInitStructureObserved = false;
	int _DiagInitBoxCount = 0;
	int _DiagInitFtypCount = 0;
	int _DiagInitMoovCount = 0;
	int _DiagInitStructureError = 0;
	uint64_t _DiagInitErrorOffset = 0;
	static const int FRAGMENT_DIAG_RING_SIZE = 180;
	FragmentDiagEntry _FragmentDiagRing[FRAGMENT_DIAG_RING_SIZE] = {};
	int _FragmentDiagRingPos = 0;
	int _FragmentDiagRingCount = 0;
	static const int PACKET_DIAG_RING_SIZE = 1024;
	PacketDiagEntry _PacketDiagRing[PACKET_DIAG_RING_SIZE] = {};
	int _PacketDiagRingPos = 0;
	int _PacketDiagRingCount = 0;
	uint64_t _PacketDiagSequence = 0;
	std::chrono::steady_clock::time_point _PacketDiagEpoch = std::chrono::steady_clock::now();
	static const int MEDIA_EVENT_RING_SIZE = 128;
	MediaDiagnosticEvent _MediaEventRing[MEDIA_EVENT_RING_SIZE] = {};
	int _MediaEventRingPos = 0;
	int _MediaEventRingCount = 0;
	uint64_t _MediaEventSequence = 0;
	uint64_t _DiagnosticActivitySequence = 0;
	uint64_t _CurrentDiagnosticActivity = 0;
	std::vector<StreamingDiagnostics::AnomalyCapture> _DiagAnomalies;
	uint64_t _DiagAnomalySequence = 0;
};

}}
