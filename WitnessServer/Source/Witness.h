#pragma once

#include "Common.h"
#include "Messages.h"
#include "AsyncWorker.h"
#include "WatchdogWorker.h"

class WitnessListener;
class GlobalContext;
class MessageBusQueue;
class CameraWorker;
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

	bool Initialize();

	void MessageLoop();

	void Shutdown();

private:

	void StatusMessage( int Camera, string_t NewStatus, string_t Reason );

	void HandleCameraStartupMessage(const CameraStartupMessage& Data);
	void HandleCameraReconnectMessage(const CameraReconnectMessage& Data);
	void HandleCameraConnectedMessage(const CameraConnectedMessage& Data);
	void HandleCameraSnapshotMessage(const CameraSnapshotMessage& Data);
	void HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data);
	void HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data);
	void HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data);

	void LoadAndroidSettings( const json::value& JsonAndroidSettings );
	bool CreateListener( const json::value& JsonServerSettings );
	bool InitializeContext();

	void StartCameraWorkers();

	void StartCameraRecording( const shared_ptr<CameraWorker>& Worker, int CameraID, bool IsManual );
	void StopCameraRecording( const ClipStatistics& ClipStats, int CameraID );

	unique_ptr<AsyncWorker> Worker;
	unique_ptr<WatchdogWorker> Watchdog;
	unique_ptr<WitnessListener>	Server;
	shared_ptr<GlobalContext> Context;
	shared_ptr<MessageBusQueue> MessageClient;

	AndroidSettings	Android;

	string_t CachePath;
};
