#pragma once

#include "Common.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>
#include "Messages.h"

#include <chrono>

#include <opencv2/opencv.hpp>

#include "MessageBus.h"

class MessageBus;

using namespace Witness::Camera;

// Callback for each detection frame with ROI data (normalized 0-1 coordinates)
struct DetectionFrameData
{
	int CameraID;
	double Timestamp;  // epoch seconds
	int FrameWidth;
	int FrameHeight;
	bool IsMotion;  // true if this frame triggered or is during active motion
	struct Box
	{
		unsigned int TrackingID;
		int ClassID;
		std::string ClassName;
		float Confidence;
		float X, Y, W, H;  // normalized 0-1
		// Face landmarks (normalized 0-1, only valid when HasLandmarks is true)
		bool HasLandmarks;
		float LandmarkX[5];
		float LandmarkY[5];
	};
	std::vector<Box> Boxes;

	// Full decoded BGR frame — valid only during callback invocation.
	// Used by callback to crop detection regions for storage.
	cv::Mat DecodedFrame;
};

using DetectionFrameCallback = std::function<void( const DetectionFrameData& )>;

class ObservingMotionFilter : public IRecordFilter
{
public:

	enum class MotionState
	{
		None,
		Current,
		GracePeriod,
	};

	ObservingMotionFilter( const MotionChainNode& NextChain, const int CameraID, const std::shared_ptr<MessageBus>& MessageBusIn );
	virtual ~ObservingMotionFilter();
	
	virtual bool ProcessFrame( SharedClassificationTask TaskData ) override;

	bool FlagToSaveNextFrame() { SaveNextFrame = true; }
	bool HasViewer() { return SaveNextFrame; }

	const ClipStatistics& GetClipStatistics() const { return ClipStats; }

	const ClassificationResult& GetCurrentResult() const 
	{
		std::lock_guard<std::mutex> Lock(Mutex);

		return Result;
	}

	void SetManualClipStart( uint64_t ClipStart )
	{
		ClipStats.TimestampClipStarted = std::min( ClipStart, ClipStats.TimestampClipStarted );
		WantManualThumbnail = true;
	}
	void SetManualClipEnd( uint64_t ClipEnd ) { ClipStats.TimestampClipEnded = std::max( ClipEnd, ClipStats.TimestampClipEnded ); }

	virtual void ClearStateThis() override { ClipStats.Clear(); }

	void SetPreviewTimestamps( uint64_t Large, uint64_t Small )
	{
		LastLargePreviewTimestamp = Large;
		LastSmallPreviewTimestamp = Small;
	}

	void SetDetectionCallback( DetectionFrameCallback callback )
	{
		DetectionCallback = std::move( callback );
	}

	void CreateJpegPreview( FilterFrame& Frame, std::vector<unsigned char>& OutputBuffer, unsigned int OutputWidth, int OutputQuality, std::function<void(cv::Mat&)> Action );

private:

	mutable std::mutex		Mutex;

	std::shared_ptr<MotionChainNode>	MotionChain;

	std::shared_ptr<MessageBus>	MessageBusPtr;

	uint64_t				LastLargePreviewTimestamp;
	uint64_t				LastSmallPreviewTimestamp;

	int64_t					LastPresentedTimestamp;
	int						CameraID;
	int						FrameIndex;
	int						LastMotionIndex;
	bool					SaveNextFrame;
	bool					WantManualThumbnail;

	ClassificationResult	Result;
	ClipStatistics			ClipStats;

	MotionState				State;

	DebugBind<int> DB_DrawObjectLabels;

	DetectionFrameCallback	DetectionCallback;
};
