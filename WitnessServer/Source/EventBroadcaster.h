#pragma once

#include <mutex>
#include <set>
#include <map>
#include <queue>
#include <string>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "crow.h"
#include "crow/json.h"
#include <Log.h>

// Camera-scoped events reach only connections whose user can access that
// camera. Unscoped events (cameraId <= 0) such as build:hash go to everyone.
inline bool EventVisibleTo( int cameraId, const std::set<int>& allowedCameras )
{
	return cameraId <= 0 || allowedCameras.count( cameraId ) > 0;
}

class EventBroadcaster
{
public:
	using AccessResolver = std::function<std::set<int>( int userUID )>;

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

	// Resolves the cameras a user may see. Results are cached per user until
	// InvalidateAccess(), which callers invoke after any group mapping change.
	void SetAccessResolver( AccessResolver resolver )
	{
		std::lock_guard<std::mutex> lock( m_AccessMutex );
		m_Resolver = std::move( resolver );
		m_AccessCache.clear();
	}

	void InvalidateAccess()
	{
		{
			std::lock_guard<std::mutex> lock( m_AccessMutex );
			m_AccessCache.clear();
		}
		Broadcast( "access:changed" );
	}

	std::set<int> AllowedCameras( int userUID )
	{
		std::lock_guard<std::mutex> lock( m_AccessMutex );
		auto it = m_AccessCache.find( userUID );
		if( it != m_AccessCache.end() )
			return it->second;
		std::set<int> allowed;
		if( m_Resolver )
			allowed = m_Resolver( userUID );
		m_AccessCache[userUID] = allowed;
		return allowed;
	}

	void AddConnection( crow::websocket::connection* conn, int userUID )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_Connections.insert( conn );
		m_ConnectionUsers[conn] = userUID;
		LOG_INFO( "WebSocket client connected (%d total)", (int)m_Connections.size() );
	}

	void RemoveConnection( crow::websocket::connection* conn )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_Connections.erase( conn );
		m_PendingCount.erase( conn );
		m_ConnectionUsers.erase( conn );
		LOG_INFO( "WebSocket client disconnected (%d remaining)", (int)m_Connections.size() );
	}

	// cameraId > 0 restricts delivery to users who can access that camera.
	void Broadcast( const std::string& eventType, crow::json::wvalue data, int cameraId = 0 )
	{
		crow::json::wvalue envelope;
		envelope["event"] = eventType;
		envelope["data"] = std::move( data );

		auto msg = std::make_shared<std::string>( envelope.dump() );

		{
			std::lock_guard<std::mutex> lock( m_QueueMutex );
			m_OutboundQueue.push( { std::move( msg ), cameraId } );
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
			int cameraId = 0;
			{
				std::unique_lock<std::mutex> lock( m_QueueMutex );
				m_Condition.wait_for( lock, std::chrono::milliseconds(100),
					[this]{ return !m_OutboundQueue.empty() || !m_Running; } );

				if( !m_Running && m_OutboundQueue.empty() ) break;
				if( m_OutboundQueue.empty() ) continue;

				msg = m_OutboundQueue.front().Message;
				cameraId = m_OutboundQueue.front().CameraId;
				m_OutboundQueue.pop();
			}

			// Resolve which users may receive a camera-scoped event before
			// taking the connection lock (resolution may query the database).
			std::map<int, bool> userVisible;
			if( cameraId > 0 )
			{
				std::vector<int> users;
				{
					std::lock_guard<std::mutex> lock( m_Mutex );
					for( auto& [conn, user] : m_ConnectionUsers )
						users.push_back( user );
				}
				for( int user : users )
					if( !userVisible.count( user ) )
						userVisible[user] = EventVisibleTo( cameraId, AllowedCameras( user ) );
			}

			// Get connection snapshot
			std::vector<crow::websocket::connection*> conns;
			std::vector<crow::websocket::connection*> toDisconnect;
			{
				std::lock_guard<std::mutex> lock( m_Mutex );
				for( auto* conn : m_Connections )
				{
					if( cameraId > 0 )
					{
						auto user = m_ConnectionUsers.find( conn );
						if( user == m_ConnectionUsers.end() ) continue;
						auto visible = userVisible.find( user->second );
						if( visible == userVisible.end() || !visible->second ) continue;
					}
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
					m_ConnectionUsers.erase( conn );
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
	std::map<crow::websocket::connection*, int> m_ConnectionUsers;

	// Per-user camera access cache
	std::mutex m_AccessMutex;
	AccessResolver m_Resolver;
	std::map<int, std::set<int>> m_AccessCache;

	// Outbound message queue
	std::mutex m_QueueMutex;
	std::condition_variable m_Condition;
	struct Outbound
	{
		std::shared_ptr<std::string> Message;
		int CameraId = 0;
	};
	std::queue<Outbound> m_OutboundQueue;

	// Broadcast thread
	std::atomic<bool> m_Running{ false };
	std::thread m_Thread;
};
