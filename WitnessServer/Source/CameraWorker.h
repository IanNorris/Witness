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
	: MotionFilterName()
	, DataPath()
	, FullBodyCascadeFilter()
	, FaceCascadeFilter()
	, ClipHistoryPeriod( 5.0 )
	, ExportMotionVectors( 1 )
	{}

	string_t	DataPath;
	string_t	MotionFilterName;
	string_t	FullBodyCascadeFilter;
	string_t	FaceCascadeFilter;
	double		ClipHistoryPeriod;
	int			ExportMotionVectors;
};

struct CameraSettings
{
	CameraSettings()
	: JobQueue()
	, Name()
	, Path()
	, MotionFilterName()
	, FullBodyCascadeFilter()
	, FaceCascadeFilter()
	, MDThreshold( 0.05 )
	, ID( 0 )
	, Enabled( 1 )
	, SkipFrames( 1 )
	, MDFrameHeight( 720 )
	{}

	ImageProcessingJobQueue* JobQueue;

	string_t Name;
	string_t Path;
	string_t MotionFilterName;
	string_t FullBodyCascadeFilter;
	string_t FaceCascadeFilter;
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
	, LastFrameTime( 0 )
	, IsConnected( false )
	, IsRTSP( false )
	{}

	InputStream::StreamStats GetStreamStats()
	{
		shared_ptr<InputStream> Stream = CameraStream;
		if( Stream )
		{
			return Stream->GetStats();
		}
		else
		{
			return InputStream::StreamStats();
		}
	}

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

	uint64_t LastFrameTime;

	bool IsConnected;
	bool IsRTSP;
};
