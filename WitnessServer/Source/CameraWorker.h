#pragma once

#include "WorkerBase.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <LiveOutputStream.h>
#include <MotionFilter.h>

class GlobalContext;
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

	std::string	DataPath;
	std::string	MotionFilterName;
	std::string	FullBodyCascadeFilter;
	std::string	FaceCascadeFilter;
	double		ClipHistoryPeriod;
	int			ExportMotionVectors;
};

struct CameraSettings
{
	CameraSettings()
	: JobQueue()
	, Name()
	, Path()
	, PathSub()
	, MotionFilterName()
	, FullBodyCascadeFilter()
	, FaceCascadeFilter()
	, BlackoutMaskPath()
	, FocusMaskPath()
	, MDThreshold( 0.05 )
	, ID( 0 )
	, Enabled( 1 )
	, SkipFrames( 1 )
	, MDFrameHeight( 720 )
	{}

	ImageProcessingJobQueue* JobQueue;

	std::string Name;
	std::string Path;
	std::string PathSub;
	std::string MotionFilterName;
	std::string FullBodyCascadeFilter;
	std::string FaceCascadeFilter;
	std::string BlackoutMaskPath;
	std::string FocusMaskPath;
	double MDThreshold;
	int ID;
	int Enabled;
	int SkipFrames;
	int MDFrameHeight;
};

class CameraWorker : public WorkerBase
{
public:
	CameraWorker( const VideoSettings& Video, const CameraSettings& Camera, const std::shared_ptr<MessageBus>& MessageBus, const std::shared_ptr<GlobalContext>& Context )
	: WorkerBase( MessageBus )
	, Context( Context )
	, Video( Video )
	, Camera( Camera )
	, LastFrameTime( 0 )
	, LastDeleteTime( 0 )
	, IsConnected( false )
	, IsRTSP( false )
	{
	}

	InputStream::StreamStats GetStreamStats()
	{
		std::shared_ptr<InputStream> Stream = CameraStream;
		if( Stream )
		{
			return Stream->GetStats();
		}
		else
		{
			return InputStream::StreamStats();
		}
	}

	std::shared_ptr<LiveOutputStream>& GetLiveStream()
	{
		return LiveStream;
	}

private:

	virtual void WorkerInit() override;
	virtual void WorkerShutdown() override;
	virtual void WorkerMain() override;

	void OnClipFinished(bool ManualStop);

	void CreateInputStream();

	std::shared_ptr<OutputStream> RecordStream;
	std::shared_ptr<LiveOutputStream> LiveStream;

	std::shared_ptr<InputStream> CameraStream;
	std::shared_ptr<IRecordFilter> Filter;
	std::shared_ptr<ObservingMotionFilter> Observer;

	const std::shared_ptr<GlobalContext> Context;

	VideoSettings Video;
	CameraSettings Camera;

	uint64_t LastFrameTime;
	uint64_t LastDeleteTime;

	bool IsConnected;
	bool IsRTSP;
};
