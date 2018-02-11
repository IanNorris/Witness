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

class ObservingMotionFilter : public MotionFilter
{
public:

	enum class MotionState
	{
		None,
		Current,
		GracePeriod,
	};

	ObservingMotionFilter( const int CameraID, const shared_ptr<MessageBus>& MessageBusIn );
	virtual ~ObservingMotionFilter();

	virtual ClassificationResult FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager ) override;

	bool FlagToSaveNextFrame() { SaveNextFrame = true; }

	const ClipStatistics& GetClipStatistics() const { return ClipStats; }

	void SetManualClipStart( uint64_t ClipStart ) { ClipStats.TimestampClipStarted = min( ClipStart, ClipStats.TimestampClipStarted ); }
	void SetManualClipEnd( uint64_t ClipEnd ) { ClipStats.TimestampClipEnded = max( ClipEnd, ClipStats.TimestampClipEnded ); }

	void ClearStats() { ClipStats.Clear(); }

private:

	shared_ptr<MessageBus>	MessageBusPtr;
	int						CameraID;
	int						FrameIndex;
	int						LastMotionIndex;
	bool					SaveNextFrame;

	ClipStatistics			ClipStats;

	MotionState				State;
};
