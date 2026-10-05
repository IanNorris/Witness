#pragma once

#include "Stream.h"
#include "SegmentBuffer.h"
#include "CadenceDeliveryEvidence.h"
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

class InputStream;
struct LiveOutputStreamData;
class PacketCapture;

struct LiveStreamInitSnapshot
{
	SegmentBuffer Data;
	int Generation = 0;
	std::string AudioCodec;
};

// Notification events emitted by LiveOutputStream for MSE WebSocket streaming
// Header-defined value type; the producer and consumer exchange it through
// exported methods/callbacks, but the type itself has no out-of-line symbols.
struct LiveStreamEvent
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
    static const char* GetNativeBuildIdentity();
    static const char* GetNativeBuildConfiguration();
    friend struct LiveTimestampRecoveryTestAccess;
	LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment);
	virtual ~LiveOutputStream();
	LiveOutputStream(const LiveOutputStream&) = delete;
	LiveOutputStream& operator=(const LiveOutputStream&) = delete;
	LiveOutputStream(LiveOutputStream&&) = delete;
	LiveOutputStream& operator=(LiveOutputStream&&) = delete;

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet ) override;

	int GetCurrentSegment();

	void GetSegments(std::vector<LiveStreamSegment>& OutSegments );

	SegmentBuffer GetInitSegment();

	int GetInitGeneration();

	LiveStreamInitSnapshot GetInitSnapshot();

	double GetPartialTargetDuration();

	void SetPartialTargetDuration(double Duration);

	// Timestamp repair is deliberately opt-in for camera profiles known to
	// emit jittery RTSP clocks; generic and genuine VFR streams are untouched.
	void SetTimestampNormalizationAllowed(bool Allowed);

	// Preview streams may opt into a stricter automatic path which remains
	// dormant until a stable no-B-frame cadence precedes a bounded DTS regression.
	void SetObservedTimestampRegressionRepairAllowed(bool Allowed);

	void ResetForReconnect(InputStream* NewInputStream);
	void NotifyDecodeCorruption(int ErrorFlags);
	// Administrator-only, bounded diagnostic capture of one live tier. Raw media
	// stays on the server; callers receive only the local directory path.
	bool StartPacketCapture(int CameraID, const std::string& Tier, int DurationSeconds,
		std::string& Directory);
	void CaptureInputPacket(const AVPacket* Packet, uint64_t ActivityID);

	// Observer for MSE WebSocket streaming — called on camera worker thread
	void SetEventCallback(LiveStreamEventCallback Callback);

private:
	std::shared_ptr<PacketCapture> ActivePacketCapture() const;

	CameraStreamError InitFormatContext();
	CameraStreamError WritePacketWithKnownDuration( const AVPacket* Packet,
		uint64_t ActivityID, int64_t SourceDuration = INT64_MIN );
	CameraStreamError StartNewSegment(const AVPacket* Packet);
	CameraStreamError FinishCurrentSegment(int64_t NextKeyframeDTS);
	bool FlushPartialSegment(bool IsIndependent);
	void PublishDecodeRecovery(int SegmentIndex, int PartialIndex);
	void MarkStreamEstablished();

	void SetupMemoryIO();

	static int WriteBuffer(void* Opaque, const uint8_t* Buffer, int BufferSize);
	static int64_t SeekBuffer(void* Opaque, int64_t Offset, int Origin);

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
		int64_t LastTimestampUnixMs = 0;
		int64_t LastElapsedMs = 0;
		uint64_t Count = 1;
		int Generation = 0;
		int SegmentIndex = 0;
		int PartialIndex = 0;
		std::string Category;
		std::string Severity;
		std::string Phase;
		std::string Component;
		std::string Message;
		std::string LastMessage;
		std::string ClusterSignature; // internal grouping key; never exported
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
        bool TimestampNormalizationRejected = false;
        unsigned TimestampRetryAttempts = 0;
        uint64_t TimestampPauseResets = 0;
        int64_t TimestampLastPauseGapMs = 0;
        int64_t TimestampRejectedUnixMs = 0;
        double TimestampDeclaredWindowMs = 0;
        double TimestampReferenceWindowMs = 0;
        Witness::Camera::CadenceDeliveryEvidence TimestampDelivery;
        std::string TimestampRejectionClock;
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
		bool StreamEstablished = false;
		int64_t StartupGraceElapsedMs = 0;
		uint64_t StartupAcceptedVideoPackets = 0;
		uint64_t StartupDroppedVideoPackets = 0;
		uint64_t StartupRepairedVideoTimestamps = 0;
		uint64_t EstablishedAcceptedVideoPackets = 0;
		uint64_t EstablishedDroppedVideoPackets = 0;
		uint64_t EstablishedRepairedVideoTimestamps = 0;
		double VideoPhaseErrorMs = 0.0;
		double VideoCorrectionMs = 0.0;
		double AudioVideoSkewMs = 0.0;
		bool HasAudioVideoSkew = false;
		uint64_t TimestampCorrectionSaturatedPackets = 0;
		uint64_t TimestampArrivalRateAdjustments = 0;
		double TimestampArrivalDurationMs = 0;
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
		bool PacketTraceEnabled = true;
		bool MuxTraceEnabled = true;
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
	uint64_t RecordFFmpegLog( int Level, const char* Phase, const char* Component,
		const char* Message, uint64_t ActivityID );

private:
	LiveOutputStreamData* const _LiveData;
};

}}
