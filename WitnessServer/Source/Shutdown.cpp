#include "Common.h"
#include "Witness.h"
#include "Listener.h"

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
		lock_guard<mutex> Lock(Context->Mutex);

		for (auto& Camera : Context->Cameras)
		{
			Camera.second.Worker->RequestShutdown();
		}
	}

	{
		for (auto& Camera : Context->Cameras)
		{
			Camera.second.Worker->Join();
		}
	}

	MessageClient->Push(make_shared<ThreadShutdownMessage>());

	Context->MessageBus->SendToClient(nullptr, make_shared<ThreadShutdownMessage>());
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