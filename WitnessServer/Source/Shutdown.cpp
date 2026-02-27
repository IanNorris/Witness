#include "Common.h"
#include "Witness.h"
#include "CrowListener.h"

void WitnessServer::Shutdown()
{
	Worker = nullptr;

	Watchdog = nullptr;

	Timer = nullptr;

	ImageWorkers.clear();

	Context->MessageBus->RemoveClient(nullptr);
	MessageClient = nullptr;
}

void WitnessServer::RequestShutdown()
{
	Server->Stop();

	{
		std::lock_guard<std::mutex> Lock(Context->Mutex);

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
	for (auto& Worker : ImageWorkers)
	{
		Worker->RequestShutdown();
	}
	for (auto& Worker : ImageWorkers)
	{
		CommonImageProcessingJobQueue.RequestShutdown();
		CommonImageProcessingJobQueue.Push(nullptr, true);
	};
}