#pragma once
// Camera-DLL-owned state. Not part of the consumer-facing header/ABI.
#include "LiveOutputStream.h"
#include "TimestampCadenceClock.h"
#include "TimestampArrivalGuard.h"
#include "TimestampRequalification.h"
#include "TimestampPauseRecovery.h"
#include "RecentWrittenVideoPackets.h"
#include "FFmpegLogClusters.h"
#include <mutex>

namespace Witness::Camera {
struct PacketCaptureState;
struct LiveOutputStreamData
{
    using SegmentDiagEntry = LiveOutputStream::SegmentDiagEntry;
    using FragmentDiagEntry = LiveOutputStream::FragmentDiagEntry;
    using PacketDiagEntry = LiveOutputStream::PacketDiagEntry;
    using MediaDiagnosticEvent = LiveOutputStream::MediaDiagnosticEvent;
    using StreamingDiagnostics = LiveOutputStream::StreamingDiagnostics;
    LiveOutputStreamData(const std::string& LiveCachePath, InputStream* InputStream);
    ~LiveOutputStreamData();
    LiveOutputStreamData(const LiveOutputStreamData&) = delete;
    LiveOutputStreamData& operator=(const LiveOutputStreamData&) = delete;

	PacketCaptureState* _PacketCaptureState = nullptr;
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
	bool _AllowObservedTimestampRegressionRepair;
	bool _NormalizeNoBFrameTimestamps;
	bool _TimestampNormalizationRejected;
    TimestampRequalification _TimestampRequalification;
    TimestampPauseRecovery _TimestampPauseRecovery;
    uint64_t _DiagTimestampPauseResets = 0;
    int64_t _DiagTimestampLastPauseGapMs = 0;
    TimestampArrivalGuard _RequalificationArrivalGuard;
    bool _RequalificationCollecting = false;
    bool _DiagTimestampRejected = false;
    unsigned _DiagTimestampRetryAttempts = 0;
    int64_t _DiagTimestampRejectedUnixMs = 0;
    double _DiagTimestampDeclaredWindowMs = 0;
    double _DiagTimestampReferenceWindowMs = 0;
    Witness::Camera::CadenceDeliveryEvidence _TimestampDeliveryEvidence;
    Witness::Camera::CadenceDeliveryEvidence _DiagTimestampDeliveryEvidence;
    bool _DiagTimestampRejectionUsesArrival = false;

	int64_t _InitialDTS;
	int64_t _InitialTimestampUs;
	int64_t _LastInputDTS;
	int64_t _LastPacketDuration;
	int64_t _LastOutputPacketDuration;
	int64_t _LastWrittenDTS;
	int64_t _LastWrittenAudioDTS;
	int _AacFrameSamples = 0;
	int64_t _NextAacSample = -1;
	int _AudioInputStreamIndex;
	int64_t _SegmentStartDTS;
	int64_t _OutputSegmentStartDTS;
	double _CurrentSegmentDuration;
	double _CurrentSegmentNominalDuration;
	int _TimestampProbeSamples;
	int _TimestampProbeOutliers;
	int64_t _TimestampProbeInputTicks;
	int64_t _TimestampProbeDurationTicks;
	TimestampCadenceClock _TimestampCadenceClock;
	TimestampArrivalGuard _TimestampArrivalGuard;
	int _TimestampStableCadenceSamples;
	int64_t _TimestampStableCadenceTicks;
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
	uint64_t _DecodeCorruptionActivityID = 0;
	bool _RecoveryPendingPublication = false;
	int _RecoverySegmentIndex = -1;
	int _RecoveryPartialIndex = -1;
	uint64_t _RecoveryActivityID = 0;
	uint64_t _RecoveryPacketSequence = 0;
	AVPacket* _PendingVideoPacket = nullptr;
	uint64_t _PendingVideoActivityID = 0;
	std::chrono::steady_clock::time_point _StartupGraceStarted;
	bool _StreamEstablished = false;

	int _InitGeneration; // incremented on reconnect so HLS.js refetches init segment

	std::chrono::time_point<std::chrono::system_clock> _CurrentSegmentWallTime;

	std::mutex* _SegmentsMutex;

	// MSE WebSocket callback — notifies subscribers of new data
	LiveStreamEventCallback _EventCallback;

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
	uint64_t _StartupAcceptedVideoPackets = 0;
	uint64_t _StartupDroppedVideoPackets = 0;
	uint64_t _StartupRepairedVideoTimestamps = 0;
	uint64_t _GenerationAcceptedVideoPacketsBaseline = 0;
	uint64_t _GenerationDroppedVideoPacketsBaseline = 0;
	uint64_t _GenerationRepairedVideoTimestampsBaseline = 0;
	uint64_t _EstablishedAcceptedVideoPacketsBaseline = 0;
	uint64_t _EstablishedDroppedVideoPacketsBaseline = 0;
	uint64_t _EstablishedRepairedVideoTimestampsBaseline = 0;
	double _DiagVideoPhaseErrorMs = 0.0;
	double _DiagVideoCorrectionMs = 0.0;
	int64_t _DiagLastVideoOutputUs = 0;
	int64_t _DiagLastAudioOutputUs = 0;
	bool _DiagHasVideoOutputTimestamp = false;
	bool _DiagHasAudioOutputTimestamp = false;
	uint64_t _DiagTimestampCorrectionSaturatedPackets = 0;
	uint64_t _DiagTimestampArrivalRateAdjustments = 0;
	double _DiagTimestampArrivalDurationMs = 0;
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
	static_assert(PACKET_DIAG_RING_SIZE == RecentWrittenVideoPackets::PacketHorizon);
	RecentWrittenVideoPackets _RecentWrittenVideoPackets;
	// Optional observer only. Functional payload history, clocks and corruption
	// handling remain active. Read once when this stream instance is created.
	bool _RetainPacketTrace = true;
	bool _RetainMuxTrace = true;
	bool _RetainMediaTrace = true;
	FFmpegLogClusters _FFmpegLogClusters;
	// Summary state must not depend on optional retained segment history.
	bool _LastCompletedSegmentNormalizationActive = false;
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
}
