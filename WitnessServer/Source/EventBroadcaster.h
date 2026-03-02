#pragma once

#include <mutex>
#include <set>
#include <string>
#include "crow.h"
#include "crow/json.h"
#include <Log.h>

class EventBroadcaster
{
public:
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
		LOG_INFO( "WebSocket client disconnected (%d remaining)", (int)m_Connections.size() );
	}

	void Broadcast( const std::string& eventType, crow::json::wvalue data )
	{
		crow::json::wvalue envelope;
		envelope["event"] = eventType;
		envelope["data"] = std::move( data );
		std::string msg = envelope.dump();

		std::lock_guard<std::mutex> lock( m_Mutex );
		for( auto* conn : m_Connections )
		{
			try
			{
				conn->send_text( msg );
			}
			catch( ... )
			{
				// Connection may be closing; ignore
			}
		}
	}

	// Convenience: broadcast simple event with no extra data
	void Broadcast( const std::string& eventType )
	{
		crow::json::wvalue empty;
		Broadcast( eventType, std::move( empty ) );
	}

private:
	std::mutex m_Mutex;
	std::set<crow::websocket::connection*> m_Connections;
};
