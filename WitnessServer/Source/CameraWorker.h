#pragma once

#include "WorkerBase.h"
#include "SubStreamWorker.h"

#include <InputStream.h>
#include <OutputStream.h>
#include <LiveOutputStream.h>
#include <ContinuousOutputStream.h>
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
	, DetectionEnabled( false )
	, DetectionModelPath()
	, DetectionConfidence( 0.5 )
	, DetectionMaxFPS( 2.0 )
	, DetectionUseGPU( false )
	, MsePartialDuration( 0.15 )
	, FaceDetectionEnabled( false )
	, FaceDetectionConfidence( 0.7 )
	, FaceBurstDuration( 3.0 )
	, FaceRecognitionEnabled( false )
	, FaceRecognitionConfidence( 0.6 )
	, FaceRecognitionMinVerified( 2 )
	, FaceRecognitionAutoAssign( false )
	{}

	std::string	DataPath;
	std::string	MotionFilterName;
	std::string	FullBodyCascadeFilter;
	std::string	FaceCascadeFilter;
	double		ClipHistoryPeriod;
	int			ExportMotionVectors;

	bool		DetectionEnabled;
	std::string	DetectionModelPath;
	double		DetectionConfidence;
	double		DetectionMaxFPS;
	bool		DetectionUseGPU;
	std::string	DetectionCudnnPath;
	double		MsePartialDuration;

	bool		FaceDetectionEnabled;
	std::string	FaceDetectionModelPath;
	double		FaceDetectionConfidence;
	double		FaceBurstDuration;

	bool		FaceRecognitionEnabled;
	std::string	FaceRecognitionModelPath;
	double		FaceRecognitionConfidence;
	int			FaceRecognitionMinVerified;
	bool		FaceRecognitionAutoAssign;
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
	, ContinuousRecording( 0 )
	, LowLatencyHLS( 0 )
	, MotionSourceCameraId( 0 )
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
	int ContinuousRecording;
	int LowLatencyHLS;
	int MotionSourceCameraId; // If set, this camera's motion events come from the source camera
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
	, m_AuthFailureBackoff( 3000 )
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

	// Returns the video codec name (e.g. "h264", "hevc") or empty if not connected
	std::string GetVideoCodecName() const;

	std::shared_ptr<LiveOutputStream>& GetLiveStream()
	{
		return LiveStream;
	}

	SubStreamWorker* GetSubStreamWorker() const
	{
		return m_SubStreamWorker.get();
	}

	std::shared_ptr<LiveOutputStream> GetSubStreamLive() const
	{
		if (m_SubStreamWorker)
			return m_SubStreamWorker->GetLiveStream();
		return nullptr;
	}

	const CameraSettings& GetCameraSettings() const
	{
		return Camera;
	}

	void SetLowLatencyHLS( int Value )
	{
		Camera.LowLatencyHLS = Value;
	}

private:

	virtual void WorkerInit() override;
	virtual void WorkerShutdown() override;
	virtual void WorkerMain() override;

	void OnClipFinished(bool ManualStop);

	void CreateInputStream();

	std::shared_ptr<OutputStream> RecordStream;
	std::shared_ptr<LiveOutputStream> LiveStream;
	std::shared_ptr<ContinuousOutputStream> ContinuousStream;

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
	int m_AuthFailureBackoff; // milliseconds, grows exponentially on auth failures

	std::unique_ptr<SubStreamWorker> m_SubStreamWorker;
};
