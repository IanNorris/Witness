#include "Common.h"
#include "Witness.h"
#include "CrowListener.h"

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
}
