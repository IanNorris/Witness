#pragma once

#include <mutex>
#include <condition_variable>
#include <vector>
#include "Message.h"

class MessageBusQueue
{
public:
	
	void Push( const std::shared_ptr<Message>& Message );
	bool TryPop(std::shared_ptr<Message>& Message );
	void Pop(std::shared_ptr<Message>& Message );

	void TryPop( std::function< void(std::shared_ptr<Message>) > );
	void Pop( std::function< void(std::shared_ptr<Message>) > );

private:

	mutable std::mutex Mutex;
	std::condition_variable Condition;

	std::vector< std::shared_ptr<Message> > Queue;
};

class MessageBus
{
public:

	std::shared_ptr<MessageBusQueue> AddClient( void* Client )
	{
		std::lock_guard<std::mutex> lock( Mutex );

		if (Clients.find(Client) == Clients.end())
		{
			auto Queue = std::make_shared<MessageBusQueue>();
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
		std::lock_guard<std::mutex> lock( Mutex );

		if (Clients.find(Client) != Clients.end())
		{
			Clients.erase( Client );
		}
		else
		{
			throw "Client not found in map";
		}
	}

	bool SendToClient( void* ClientTo, const std::shared_ptr<Message>& Message )
	{
		std::shared_ptr<MessageBusQueue> Queue;

		{
			std::lock_guard<std::mutex> lock( Mutex );

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

	template<class T>
	void Forward( void* ClientTo, const std::shared_ptr<Message>& Message )
	{
		T* Object = dynamic_cast<T*>(Message.get());
		if (Object)
		{
			SendToClient( ClientTo, Message );
		}
	}

private:

	mutable std::mutex Mutex;

	std::unordered_map< void*, std::shared_ptr<MessageBusQueue> > Clients;
};
