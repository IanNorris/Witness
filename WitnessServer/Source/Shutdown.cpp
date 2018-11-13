#include "Common.h"
#include "Witness.h"
#include "Listener.h"

void WitnessServer::Shutdown()
{
	Context->MessageBus->RemoveClient( nullptr );
	MessageClient = nullptr;

	Worker->RequestShutdown();
	Worker = nullptr;

	Watchdog->RequestShutdown();
	Watchdog = nullptr;

	Timer->RequestShutdown();
	Timer = nullptr;

	for (auto& Worker : ImageWorkers)
	{
		Worker->RequestShutdown();
	}
	for (auto& Worker : ImageWorkers)
	{
		CommonImageProcessingJobQueue.Push(nullptr);
	};
	ImageWorkers.clear();

	Server->Stop();
}
