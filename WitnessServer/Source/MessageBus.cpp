#include "MessageBus.h"

void MessageBusQueue::Push( const shared_ptr<Message>& Message )
{
	unique_lock<mutex> Lock( Mutex );

	Queue.push_back( Message );

	Condition.notify_one();
}

bool MessageBusQueue::TryPop( shared_ptr<Message>& Message )
{
	unique_lock<mutex> Lock( Mutex );

	if( !Queue.empty() )
	{
		Message = Queue.front();
		Queue.erase( Queue.begin() );
		return true;
	}

	return false;
}

void MessageBusQueue::Pop( shared_ptr<Message>& Message )
{
	unique_lock<mutex> Lock( Mutex );

	while( Queue.empty() )
	{
		Condition.wait( Lock );
	}

	Message = Queue.front();
	Queue.erase( Queue.begin() );
}
