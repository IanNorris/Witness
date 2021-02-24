#pragma once

#include "Common.h"
#include "Messages.h"
#include "AsyncWorker.h"
#include "TimerWorker.h"
#include "ImageProcessorWorker.h"
#include "CameraWorker.h"
#include "WatchdogWorker.h"
#include "CameraState.h"
#include <ImageProcessingJob.h>

class WitnessListener;
class GlobalContext;
struct CameraState;
class MessageBusQueue;
struct ClipStatistics;

struct AndroidSettings
{
	AndroidSettings()
	: UseAndroid(false)
	{}

	string_t	ServerKey;
	string_t	TempUserId;
	bool		UseAndroid;
};

class WitnessServer
{
public:

	bool Initialize( DebugConsole* DebugConsoleInstance );

	void MessageLoop( bool& ContinueRunning );

	void Shutdown();
	void RequestShutdown();

private:

	void StatusMessage( int Camera, string_t NewStatus, string_t Reason );

	void HandleCameraStartupMessage(const CameraStartupMessage& Data);
	void HandleCameraReconnectMessage(const CameraReconnectMessage& Data);
	void HandleCameraConnectedMessage(const CameraConnectedMessage& Data);
	void HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data);
	void HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data);
	void HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data);
	void HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data);

	void LoadAndroidSettings( const std::unordered_map< string_t, string_t >& Settings );
	bool CreateListener( const std::unordered_map< string_t, string_t >& Settings );
	bool CreateProcessors( const std::unordered_map< string_t, string_t >& Settings );
	bool InitializeContext( const std::shared_ptr<SQLiteDatabase>& Database );

	void HandleActions( const shared_ptr<GlobalContext>& Context, CameraState& State, int CameraIndex, double MotionThreshold );
	void TriggerAction( const string_t& Command, const string_t& Param1, const string_t& Param2, const string_t& Param3, CameraState& State, int CameraIndex );

	void StartCameraWorkers();

	void StartCamera(const SQLiteDatabaseQuery& query);

	void StartCameraRecording( const shared_ptr<CameraWorker>& Worker, uint64_t Timestamp, int CameraID, bool IsManual, const ClassificationResult& Result );
	void StopCameraRecording( const ClipStatistics& ClipStats, int CameraID, const ClassificationResult& Result );

	unique_ptr<AsyncWorker> Worker;
	unique_ptr<WatchdogWorker> Watchdog;
	unique_ptr<TimerWorker> Timer;
	unique_ptr<WitnessListener>	Server;
	shared_ptr<GlobalContext> Context;
	shared_ptr<MessageBusQueue> MessageClient;

	DebugConsole* DebugConsoleInstance;

	vector<shared_ptr<ImageProcessorWorker>> ImageWorkers;

	Witness::Camera::ImageProcessingJobQueue CommonImageProcessingJobQueue;

	AndroidSettings	Android;
	VideoSettings Video;

	string_t CachePath;
};
