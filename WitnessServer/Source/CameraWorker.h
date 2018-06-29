#pragma once

#include "WorkerBase.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <MotionFilter.h>

class ObservingMotionFilter;

using namespace Witness::Camera;

struct VideoSettings
{
	VideoSettings()
	: ClipHistoryPeriod( 5.0 )
	{}

	double		ClipHistoryPeriod;
};

struct CameraSettings
{
	CameraSettings()
	: JobQueue()
	, Name()
	, Path()
	, MDThreshold( 0.05 )
	, ID( 0 )
	, Enabled( 1 )
	, SkipFrames( 1 )
	, MDFrameHeight( 720 )
	{}

	ImageProcessingJobQueue* JobQueue;

	string_t Name;
	string_t Path;
	double MDThreshold;
	int ID;
	int Enabled;
	int SkipFrames;
	int MDFrameHeight;
};

class CameraWorker : public WorkerBase
{
public:
	CameraWorker( const VideoSettings& Video, const CameraSettings& Camera, const shared_ptr<MessageBus>& MessageBus)
	: WorkerBase( MessageBus )
	, Video( Video )
	, Camera( Camera )
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

	VideoSettings Video;
	CameraSettings Camera;

	bool IsConnected;
};
