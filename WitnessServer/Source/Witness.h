#pragma once

#include "Common.h"
#include "Messages.h"
#include "AsyncWorker.h"
#include "TimerWorker.h"
#include "ImageProcessorWorker.h"
#include "CameraWorker.h"
#include "WatchdogWorker.h"
#include "ClipReprocessWorker.h"
#include "CameraState.h"
#include <ImageProcessingJob.h>
#include "OperationalStatus.h"
#include <mutex>
#include <unordered_map>

class CrowListener;
class GlobalContext;
struct CameraState;
class MessageBusQueue;
struct ClipStatistics;

class WitnessServer
{
public:

	bool Initialize( DebugConsole* DebugConsoleInstance );

	void MessageLoop( bool& ContinueRunning );

	void Shutdown();
	void RequestShutdown();
	OperationalStatus GetOperationalStatus() const;

private:
	struct OperationalCameraFlags
	{
		std::string State = "Starting";
		bool Recording = false;
		bool MotionActive = false;
	};

	void SetOperationalState( int Camera, const std::string& State );
	void SetOperationalActivity( int Camera, bool Recording, bool MotionActive );
	void SetOperationalRecording( int Camera, bool Recording );
	void SetOperationalMotion( int Camera, bool MotionActive );


	void StatusMessage( int Camera, std::string NewStatus, std::string Reason );

	void HandleCameraStartupMessage(const CameraStartupMessage& Data);
	void HandleCameraReconnectMessage(const CameraReconnectMessage& Data);
	void HandleCameraConnectedMessage(const CameraConnectedMessage& Data);
	void HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data);
	void HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data);
	void HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data);
	void HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data);

	bool CreateListener( const std::unordered_map< std::string, std::string >& Settings );
	bool CreateProcessors( const std::unordered_map< std::string, std::string >& Settings );
	bool InitializeContext( const std::shared_ptr<SQLiteDatabase>& Database );



	void StartCameraWorkers();

	void StartCamera(const SQLiteDatabaseQuery& query);

	void StartCameraRecording( const std::shared_ptr<CameraWorker>& Worker, uint64_t Timestamp, int CameraID, bool IsManual, const ClassificationResult& Result );
	void StopCameraRecording( const ClipStatistics& ClipStats, int CameraID, const ClassificationResult& Result );

	std::unique_ptr<AsyncWorker> Worker;
	std::unique_ptr<WatchdogWorker> Watchdog;
	std::unique_ptr<TimerWorker> Timer;
	std::unique_ptr<ClipReprocessWorker> ReprocessWorker;
	std::unique_ptr<CrowListener>	Server;
	std::shared_ptr<GlobalContext> Context;
	std::shared_ptr<MessageBusQueue> MessageClient;

	DebugConsole* DebugConsoleInstance;

	std::vector<std::shared_ptr<ImageProcessorWorker>> ImageWorkers;

	Witness::Camera::ImageProcessingJobQueue CommonImageProcessingJobQueue;

	VideoSettings Video;

	std::string CachePath;
	bool AllCamerasReported = false;
	mutable std::mutex OperationalMutex;
	std::unordered_map<int, OperationalCameraFlags> OperationalCameraStates;
};
