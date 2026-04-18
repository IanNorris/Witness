#pragma once

#include <mutex>
#include <set>
#include <map>
#include <queue>
#include <string>
#include <thread>
#include <condition_variable>
#include <atomic>
#include "crow.h"
#include "crow/json.h"
#include <Log.h>

class EventBroadcaster
{
public:
	static constexpr size_t MaxPendingPerClient = 64;

	~EventBroadcaster()
	{
		Stop();
	}

	void Start()
	{
		m_Running = true;
		m_Thread = std::thread( &EventBroadcaster::BroadcastLoop, this );
	}

	void Stop()
	{
		m_Running = false;
		m_Condition.notify_one();
		if( m_Thread.joinable() )
			m_Thread.join();
	}

	void AddConnection( crow::websocket::connection* conn )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_Connections.insert( conn );
		LOG_INFO( "WebSocket client connected (%d total)", (int)m_Connections.size() );
	}

	void RemoveConnection( crow::websocket::connection* conn )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_Connections.erase( conn );
		m_PendingCount.erase( conn );
		LOG_INFO( "WebSocket client disconnected (%d remaining)", (int)m_Connections.size() );
	}

	void Broadcast( const std::string& eventType, crow::json::wvalue data )
	{
		crow::json::wvalue envelope;
		envelope["event"] = eventType;
		envelope["data"] = std::move( data );

		auto msg = std::make_shared<std::string>( envelope.dump() );

		{
			std::lock_guard<std::mutex> lock( m_QueueMutex );
			m_OutboundQueue.push( std::move( msg ) );
		}
		m_Condition.notify_one();
	}

	// Convenience: broadcast simple event with no extra data
	void Broadcast( const std::string& eventType )
	{
		crow::json::wvalue empty;
		Broadcast( eventType, std::move( empty ) );
	}

private:
	void BroadcastLoop()
	{
		while( m_Running )
		{
			std::shared_ptr<std::string> msg;
			{
				std::unique_lock<std::mutex> lock( m_QueueMutex );
				m_Condition.wait_for( lock, std::chrono::milliseconds(100),
					[this]{ return !m_OutboundQueue.empty() || !m_Running; } );

				if( !m_Running && m_OutboundQueue.empty() ) break;
				if( m_OutboundQueue.empty() ) continue;

				msg = m_OutboundQueue.front();
				m_OutboundQueue.pop();
			}

			// Get connection snapshot
			std::vector<crow::websocket::connection*> conns;
			std::vector<crow::websocket::connection*> toDisconnect;
			{
				std::lock_guard<std::mutex> lock( m_Mutex );
				for( auto* conn : m_Connections )
				{
					auto& pending = m_PendingCount[conn];
					if( pending >= MaxPendingPerClient )
					{
						toDisconnect.push_back( conn );
					}
					else
					{
						pending++;
						conns.push_back( conn );
					}
				}

				// Remove slow clients
				for( auto* conn : toDisconnect )
				{
					m_Connections.erase( conn );
					m_PendingCount.erase( conn );
				}
			}

			// Disconnect slow clients (outside lock)
			for( auto* conn : toDisconnect )
			{
				LOG_WARNING( "WebSocket: disconnecting slow client (>%zu pending messages)", MaxPendingPerClient );
				try { conn->close( "slow consumer" ); } catch( ... ) {}
			}

			// Send to healthy clients (outside lock)
			for( auto* conn : conns )
			{
				try
				{
					conn->send_text( *msg );
				}
				catch( ... )
				{
					// Connection closing -- will be cleaned up via RemoveConnection
				}

				// Decrement pending count
				std::lock_guard<std::mutex> lock( m_Mutex );
				auto it = m_PendingCount.find( conn );
				if( it != m_PendingCount.end() && it->second > 0 )
					it->second--;
			}
		}
	}

	// Connection tracking
	std::mutex m_Mutex;
	std::set<crow::websocket::connection*> m_Connections;
	std::map<crow::websocket::connection*, size_t> m_PendingCount;

	// Outbound message queue
	std::mutex m_QueueMutex;
	std::condition_variable m_Condition;
	std::queue<std::shared_ptr<std::string>> m_OutboundQueue;

	// Broadcast thread
	std::atomic<bool> m_Running{ false };
	std::thread m_Thread;
};
