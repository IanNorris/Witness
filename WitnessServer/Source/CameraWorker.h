#pragma once

#include "WorkerBase.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

class ObservingMotionFilter;

using namespace Witness::Camera;

class CameraWorker : public WorkerBase
{
public:
	CameraWorker(const int CameraID, unsigned int SkipFrames, unsigned int MotionDetectFrameHeight, double MotionDetectThreshold, ImageProcessingJobQueue* JobQueue, const string_t& InputPath, const shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	, JobQueue( JobQueue )
	, Path( InputPath )
	, MotionDetectThreshold( MotionDetectThreshold )
	, SkipFrames( SkipFrames )
	, MotionDetectFrameHeight( MotionDetectFrameHeight )
	, CameraID( CameraID )
	, IsConnected( false )
	{}

private:

	virtual void WorkerInit() override;
	virtual void WorkerShutdown() override;
	virtual void WorkerMain() override;

	void OnClipFinished();

	void CreateInputStream();

	shared_ptr<OutputStream> RecordStream;

	shared_ptr<InputStream> CameraStream;
	shared_ptr<ObservingMotionFilter> Filter;

	ImageProcessingJobQueue* JobQueue;

	string_t Path;
	double MotionDetectThreshold;
	unsigned int SkipFrames;
	unsigned int MotionDetectFrameHeight;
	int CameraID;
	bool IsConnected;
};
