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

	MessageClient->Push(make_shared<ThreadShutdownMessage>());

	Context->MessageBus->SendToClient(nullptr, make_shared<ThreadShutdownMessage>());
	Worker->RequestShutdown();
	Watchdog->RequestShutdown();
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