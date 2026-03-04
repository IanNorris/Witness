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

// Callback invoked when a segment file is finalized.
// Parameters: cameraUID, startTimestamp, endTimestamp, duration (seconds), filePath
using SegmentCompleteCallback = std::function<void(int cameraUID, int64_t startTimestamp, int64_t endTimestamp, int duration, const std::string& filePath)>;

class CAMERA_API ContinuousOutputStream
{
public:
	ContinuousOutputStream(const std::string& basePath, int cameraUID, InputStream* inputStream);
	~ContinuousOutputStream();

	// Set callback for when a segment is finalized
	void SetSegmentCompleteCallback(SegmentCompleteCallback callback);

	// Target segment duration in seconds (actual will be longer, split on next keyframe)
	void SetTargetSegmentDuration(int seconds);

	// Write a video packet. Audio packets are skipped.
	CameraStreamError WritePacket(const AVPacket* packet);

	// Finalize the current segment (e.g. on disconnect or shutdown)
	void Finalize();

	// Reset for camera reconnect — finalize current and prepare for new stream
	void ResetForReconnect(InputStream* newInputStream);

private:
	CameraStreamError StartNewSegment();
	CameraStreamError FinalizeCurrentSegment();

	std::string m_BasePath;		// Directory: CachePath/continuous/{CameraID}/
	int m_CameraUID;
	InputStream* m_InputStream;

	// Current segment state
	AVFormatContext* m_FormatContext;
	AVStream* m_OutStream;
	bool m_SegmentOpen;
	int64_t m_SegmentStartTimestamp;	// Unix timestamp when segment started
	int64_t m_FirstDTS;				// First DTS of current segment (for normalization)
	int64_t m_LastWrittenDTS;
	double m_SegmentDuration;			// Accumulated duration in seconds

	int m_TargetSegmentDuration;		// Target duration before looking for next keyframe
	bool m_WaitingForKeyframe;			// Set when duration exceeded, waiting for next keyframe to split

	SegmentCompleteCallback m_OnSegmentComplete;

	char m_ErrorMessage[256];
};

}}
