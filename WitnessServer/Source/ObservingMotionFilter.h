#pragma once

#include "Common.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>
#include "Messages.h"

#include <cpprest/asyncrt_utils.h>

#include <opencv2/opencv.hpp>

#include "MessageBus.h"

class MessageBus;

using namespace Witness::Camera;

class ObservingMotionFilter : public IRecordFilter
{
public:

	enum class MotionState
	{
		None,
		Current,
		GracePeriod,
	};

	ObservingMotionFilter( const MotionChainNode& NextChain, const int CameraID, const shared_ptr<MessageBus>& MessageBusIn );
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

	void SetManualClipStart( uint64_t ClipStart ) { ClipStats.TimestampClipStarted = min( ClipStart, ClipStats.TimestampClipStarted ); }
	void SetManualClipEnd( uint64_t ClipEnd ) { ClipStats.TimestampClipEnded = max( ClipEnd, ClipStats.TimestampClipEnded ); }

	virtual void ClearStateThis() override { ClipStats.Clear(); }

	void SetPreviewTimestamps( uint64_t Large, uint64_t Small )
	{
		LastLargePreviewTimestamp = Large;
		LastSmallPreviewTimestamp = Small;
	}

	void CreateJpegPreview( FilterFrame& Frame, vector<unsigned char>& OutputBuffer, unsigned int OutputWidth, int OutputQuality, std::function<void(cv::Mat&)> Action );

private:

	mutable std::mutex		Mutex;

	shared_ptr<MotionChainNode>	MotionChain;

	shared_ptr<MessageBus>	MessageBusPtr;

	uint64_t				LastLargePreviewTimestamp;
	uint64_t				LastSmallPreviewTimestamp;

	int64_t					LastPresentedTimestamp;
	int						CameraID;
	int						FrameIndex;
	int						LastMotionIndex;
	bool					SaveNextFrame;

	ClassificationResult	Result;
	ClipStatistics			ClipStats;

	MotionState				State;

	DebugBind<int> DB_DrawObjectLabels;
};
