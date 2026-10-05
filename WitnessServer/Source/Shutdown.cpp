#include "Common.h"
#include "Witness.h"
#include "CrowListener.h"
#include "EventBroadcaster.h"
#include "StreamBroadcaster.h"

void WitnessServer::Shutdown()
{
	DetectionCleanupThread.request_stop();
	if( DetectionCleanupThread.joinable() ) DetectionCleanupThread.join();
	if( Context && Context->Events )
		Context->Events->Stop();

	if( Context && Context->Streams )
		Context->Streams->Stop();

	Worker = nullptr;

	Watchdog = nullptr;

	Timer = nullptr;

	ReprocessWorker = nullptr;
	AudioWorker = nullptr;

	ImageWorkers.clear();

	Context->MessageBus->RemoveClient(nullptr);
	MessageClient = nullptr;
}

void WitnessServer::RequestShutdown()
{
	DetectionCleanupThread.request_stop();
	if( DetectionCleanupThread.joinable() ) DetectionCleanupThread.join();
	Server->Stop();
	// HTTP handlers are joined; stop held PTZ gestures while camera transports
	// still exist, before camera workers begin disconnecting.
	Context->PtzMovement.reset();

	{
		std::unique_lock<std::shared_mutex> Lock(Context->Mutex);

		for (auto& Camera : Context->GetCameraMap())
		{
			Camera.second.Worker->RequestShutdown();
		}
	}

	{
		for (auto& Camera : Context->GetCameraMap())
		{
			Camera.second.Worker->Join();
		}
	}

	MessageClient->Push(std::make_shared<ThreadShutdownMessage>());

	Context->MessageBus->SendToClient(nullptr, std::make_shared<ThreadShutdownMessage>());
	if (Worker)
	{
		Worker->RequestShutdown();
	}
	if (Watchdog)
	{
		Watchdog->RequestShutdown();
	}
	Timer->RequestShutdown();
	if (ReprocessWorker)
	{
		ReprocessWorker->RequestShutdown();
		ReprocessWorker->Join();
	}
	if( AudioWorker )
	{
		AudioWorker->RequestShutdown();
		AudioWorker->Join();
	}
	for (auto& Worker : ImageWorkers)
	{
		Worker->RequestShutdown();
	}
	for (auto& Worker : ImageWorkers)
	{
		CommonImageProcessingJobQueue.RequestShutdown();
		CommonImageProcessingJobQueue.Push(nullptr, true);
	};
	for (auto& Worker : ImageWorkers)
	{
		Worker->Join();
	}

	// Join before Shutdown destroys derived members (including timer callbacks
	// and watchdog targets) or stops their shared services. The base destructor
	// runs only after derived members have already been destroyed.
	if( Worker ) Worker->Join();
	if( Watchdog ) Watchdog->Join();
	if( Timer ) Timer->Join();

	// ContinueRunning is already false, so MessageLoop may have exited before
	// camera workers emitted their final clip statistics. Producers are joined;
	// consume one finite snapshot, processing only finalization (never camera
	// start/configuration commands). Serialize with any dispatch still finishing.
	if( MessageClient )
	{
		std::lock_guard DispatchLock(MessageDispatchMutex);
		for( const auto& Msg : MessageClient->TakePending() )
		{
			Msg->Handle<CameraClipFinishedMessage>([&](const CameraClipFinishedMessage& Data)
			{
				StopCameraRecording(Data.ClipStats, Data.Camera, Data.Result);
			});
		}
		// The snapshot may consume the original wake message before a blocked
		// MessageLoop::Pop sees it. Leave a fresh wake so it can observe stop.
		MessageClient->Push(std::make_shared<ThreadShutdownMessage>());
	}
}
