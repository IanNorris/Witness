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

	Server->Stop();
}
