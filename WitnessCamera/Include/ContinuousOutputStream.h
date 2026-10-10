#pragma once

#include "Export.h"
#include "Stream.h"

#include <string>
#include <functional>
#include <cstdint>

struct AVPacket;
struct AVRational;
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;

namespace Witness{
namespace Camera{

class InputStream;
struct ContinuousOutputStreamData;

// Immutable readable prefix of an open fragmented DVR file. Bytes after this
// boundary belong to an unfinished fragment and must never be served as media.
struct ContinuousSegmentSnapshot
{
	std::string FilePath;
	int64_t StartTimestamp = 0;
	double Duration = 0;
	int64_t CommittedBytes = 0;
	// Video successfully accepted by the writer, including the unfinished tail.
	double ObservedDuration = 0;
	// Presentation-clock coverage, independent of packet arrival/buffering.
	double MediaStartTimestamp = 0;
	double MediaEndTimestamp = 0;
};

// Callback invoked when a segment file is finalized.
// Parameters: cameraUID, startTimestamp, endTimestamp, duration (seconds), filePath
using SegmentCompleteCallback = std::function<void(int cameraUID, int64_t startTimestamp, int64_t endTimestamp, int duration, const std::string& filePath)>;

class CAMERA_API ContinuousOutputStream
{
public:
	ContinuousOutputStream(const std::string& basePath, int cameraUID, InputStream* inputStream);
	~ContinuousOutputStream();
	ContinuousOutputStream( const ContinuousOutputStream& ) = delete;
	ContinuousOutputStream& operator=( const ContinuousOutputStream& ) = delete;

	// Set callback for when a segment is finalized
	void SetSegmentCompleteCallback(SegmentCompleteCallback callback);

	// Enable before writing; defaults off until active-DVR integration is ready.
	void SetFragmentedRecordingEnabled(bool enabled);
	ContinuousSegmentSnapshot GetCommittedSegment() const;

	// Target segment duration in seconds (actual will be longer, split on next keyframe)
	void SetTargetSegmentDuration(int seconds);
	// Owning worker only, before its first packet; used for retained live history.
	void SetInitialSegmentTimestamp(int64_t timestamp) { if (!m_SegmentOpen && timestamp > 0) m_InitialSegmentTimestamp = timestamp; }
	void SetTimestampNormalizationAllowed(bool allowed)
	{
		m_TimestampNormalizationAllowed = allowed;
	}

	// Write a selected video or AAC audio packet without transcoding.
	CameraStreamError WritePacket(const AVPacket* packet);

	// Finalize the current segment (e.g. on disconnect or shutdown)
	void Finalize();

	// Reset for camera reconnect — finalize current and prepare for new stream
	void ResetForReconnect(InputStream* newInputStream);

private:
	CameraStreamError StartNewSegment(double presentationTimestamp);
	CameraStreamError FinalizeCurrentSegment();
	bool NormalizeVideoTimestamp(AVPacket* packet, AVRational inputTimebase);

	ContinuousOutputStreamData* m_Data;
	int m_CameraUID;
	InputStream* m_InputStream;

	// Current segment state
	AVFormatContext* m_FormatContext;
	AVStream* m_OutStream;
	AVStream* m_AudioOutStream;
	bool m_SegmentOpen;
	int64_t m_SegmentStartTimestamp;	// Unix timestamp when segment started
	int64_t m_FirstDTS;				// First DTS of current segment (for normalization)
	int64_t m_FirstTimestampUs;		// Common video/audio timeline origin
	int64_t m_LastWrittenDTS;
	int64_t m_LastWrittenAudioDTS;
	bool m_TimestampNormalizationAllowed;
	int64_t m_LastRawVideoDTS;
	int64_t m_LastNormalizedVideoDuration;
	int64_t m_TimestampCorrectionRemainder;
	uint64_t m_RepairedVideoTimestamps;
	bool m_TimestampNormalizationActive;
	bool m_TimestampNormalizationRejected;
	int m_TimestampProbeSamples;
	int m_TimestampProbeOutliers;
	int64_t m_TimestampProbeDeltaTicks;
	int64_t m_TimestampProbeNominalTicks;
	int64_t m_QualifiedVideoDuration;
	int64_t m_TimestampSourceOffset;
	double m_SegmentDuration;			// Accumulated duration in seconds

	int m_TargetSegmentDuration;		// Target duration before looking for next keyframe
	int64_t m_InitialSegmentTimestamp = 0;
	bool m_WaitingForKeyframe;			// Set when duration exceeded, waiting for next keyframe to split

	char m_ErrorMessage[256];
};

}}
