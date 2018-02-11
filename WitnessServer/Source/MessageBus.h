#pragma once

#include <mutex>
#include <condition_variable>
#include <vector>
#include "cpprest/json.h"
#include "Message.h"

using namespace web;
using namespace std;

class MessageBusQueue
{
public:
	
	void Push( const shared_ptr<Message>& Message );
	bool TryPop( shared_ptr<Message>& Message );
	void Pop( shared_ptr<Message>& Message );

	void TryPop( std::function< void(shared_ptr<Message>) > );
	void Pop( std::function< void(shared_ptr<Message>) > );

private:

	mutable mutex Mutex;
	condition_variable Condition;

	vector< shared_ptr<Message> > Queue;
};

class MessageBus
{
public:

	shared_ptr<MessageBusQueue> AddClient( void* Client )
	{
		lock_guard<mutex> lock( Mutex );

		if (Clients.find(Client) == Clients.end())
		{
			auto Queue = make_shared<MessageBusQueue>();
			Clients[ Client ] = Queue;

			return Queue;
		}
		else
		{
			throw "Client already found in map";
		}
	}

	void RemoveClient( void* Client )
	{
		lock_guard<mutex> lock( Mutex );

		if (Clients.find(Client) != Clients.end())
		{
			Clients.erase( Client );
		}
		else
		{
			throw "Client not found in map";
		}
	}

	bool SendToClient( void* ClientTo, const shared_ptr<Message>& Message )
	{
		shared_ptr<MessageBusQueue> Queue;

		{
			lock_guard<mutex> lock( Mutex );

			auto Iter = Clients.find( ClientTo );

			if( Iter != Clients.end() )
			{
				Queue = (*Iter).second;
			}
		}

		if( Queue )
		{
			Queue->Push( Message );

			return true;
		}

		return false;
	}

private:

	mutable mutex Mutex;

	unordered_map< void*, shared_ptr<MessageBusQueue> > Clients;
};
