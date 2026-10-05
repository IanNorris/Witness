#include "MessageBus.h"

void MessageBusQueue::Push( const std::shared_ptr<Message>& Message )
{
	std::unique_lock<std::mutex> Lock( Mutex );

	Queue.push_back( Message );

	Condition.notify_one();
}

std::vector<std::shared_ptr<Message>> MessageBusQueue::TakePending()
{
	std::vector<std::shared_ptr<Message>> Pending;
	std::lock_guard Lock(Mutex);
	Pending.swap(Queue);
	return Pending;
}

bool MessageBusQueue::TryPop(std::shared_ptr<Message>& Message )
{
	std::unique_lock<std::mutex> Lock( Mutex );

	if( !Queue.empty() )
	{
		Message = Queue.front();
		Queue.erase( Queue.begin() );
		return true;
	}

	return false;
}

void MessageBusQueue::Pop(std::shared_ptr<Message>& Message )
{
	std::unique_lock<std::mutex> Lock( Mutex );

	while( Queue.empty() )
	{
		Condition.wait( Lock );
	}

	Message = Queue.front();
	Queue.erase( Queue.begin() );
}
