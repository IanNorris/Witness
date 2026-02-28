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

private:

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

	void HandleActions( const std::shared_ptr<GlobalContext>& Context, CameraState& State, int CameraIndex, double MotionThreshold );
	void TriggerAction( const std::string& Command, const std::string& Param1, const std::string& Param2, const std::string& Param3, CameraState& State, int CameraIndex );

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
};
