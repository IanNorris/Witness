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

struct MotionChainNode
{
	shared_ptr<MotionChainNode> OnSuccess;
	shared_ptr<MotionChainNode> OnFailure;

	shared_ptr<IRecordFilter> Filter;

	unsigned int InclusiveFilter; //Mask that must be matched for success
	unsigned int ExclusiveFilter; //Mask that must not be matched for success

	float MinimumThreshold;
};

class ObservingMotionFilter : public IRecordFilter
{
public:

	enum class MotionState
	{
		None,
		Current,
		GracePeriod,
	};

	ObservingMotionFilter( const shared_ptr<MotionChainNode>& MotionChain, const int CameraID, const shared_ptr<MessageBus>& MessageBusIn );
	virtual ~ObservingMotionFilter();

	virtual void ClassifyFrame( FilterFrame& Frame, ClassificationResult& Result ) override;
	virtual void ClearState() override;

	void ClearState( MotionChainNode* Node );

	bool FlagToSaveNextFrame() { SaveNextFrame = true; }

	const ClipStatistics& GetClipStatistics() const { return ClipStats; }

	void SetManualClipStart( uint64_t ClipStart ) { ClipStats.TimestampClipStarted = min( ClipStart, ClipStats.TimestampClipStarted ); }
	void SetManualClipEnd( uint64_t ClipEnd ) { ClipStats.TimestampClipEnded = max( ClipEnd, ClipStats.TimestampClipEnded ); }

	void ClearStats() { ClipStats.Clear(); }

	void SetPreviewTimestamps( uint64_t Large, uint64_t Small )
	{
		LastLargePreviewTimestamp = Large;
		LastSmallPreviewTimestamp = Small;
	}

	void CreateJpegPreview( FilterFrame& Frame, vector<unsigned char>& OutputBuffer, unsigned int OutputWidth, int OutputQuality, std::function<void(cv::Mat&)> Action );

private:

	shared_ptr<MotionChainNode>	MotionChain;

	shared_ptr<MessageBus>	MessageBusPtr;

	uint64_t				LastLargePreviewTimestamp;
	uint64_t				LastSmallPreviewTimestamp;

	int						CameraID;
	int						FrameIndex;
	int						LastMotionIndex;
	bool					SaveNextFrame;

	ClipStatistics			ClipStats;

	MotionState				State;
};
