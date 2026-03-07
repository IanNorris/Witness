#pragma once

#include <mutex>
#include <set>
#include <map>
#include <queue>
#include <string>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <memory>
#include "crow.h"
#include "crow/json.h"
#include <Log.h>
#include <LiveOutputStream.h>

// Per-camera WebSocket broadcaster for MSE streaming.
// Each camera can have multiple WebSocket clients subscribed.
// Messages are queued and sent on a dedicated broadcast thread
// to avoid blocking camera worker threads.
class StreamBroadcaster
{
public:
	static constexpr size_t MaxPendingPerClient = 128; // Higher than EventBroadcaster — binary data is larger

	~StreamBroadcaster()
	{
		Stop();
	}

	void Start()
	{
		m_Running = true;
		m_Thread = std::thread( &StreamBroadcaster::BroadcastLoop, this );
	}

	void Stop()
	{
		m_Running = false;
		m_Condition.notify_one();
		if( m_Thread.joinable() )
			m_Thread.join();
	}

	void Subscribe( int cameraId, crow::websocket::connection* conn )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		m_Subscriptions[cameraId].insert( conn );
		m_ConnectionCamera[conn] = cameraId;
		LOG_INFO( "[MSE] Client subscribed to camera %d (%d viewers)",
			cameraId, (int)m_Subscriptions[cameraId].size() );
	}

	void Unsubscribe( crow::websocket::connection* conn )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		auto it = m_ConnectionCamera.find( conn );
		if( it != m_ConnectionCamera.end() )
		{
			int cameraId = it->second;
			m_Subscriptions[cameraId].erase( conn );
			if( m_Subscriptions[cameraId].empty() )
				m_Subscriptions.erase( cameraId );
			m_ConnectionCamera.erase( it );
			m_PendingCount.erase( conn );
			LOG_INFO( "[MSE] Client unsubscribed from camera %d", cameraId );
		}
	}

	int GetViewerCount( int cameraId ) const
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		auto it = m_Subscriptions.find( cameraId );
		return it != m_Subscriptions.end() ? (int)it->second.size() : 0;
	}

	bool HasViewers( int cameraId ) const
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		auto it = m_Subscriptions.find( cameraId );
		return it != m_Subscriptions.end() && !it->second.empty();
	}

	// Send a JSON control message to all subscribers of a camera
	void SendControl( int cameraId, const std::string& json )
	{
		auto msg = std::make_shared<BroadcastMessage>();
		msg->CameraId = cameraId;
		msg->IsBinary = false;
		msg->TextData = json;

		{
			std::lock_guard<std::mutex> lock( m_QueueMutex );
			m_OutboundQueue.push( std::move( msg ) );
		}
		m_Condition.notify_one();
	}

	// Send binary segment data to all subscribers of a camera
	void SendBinary( int cameraId, Witness::Camera::SegmentBuffer data )
	{
		auto msg = std::make_shared<BroadcastMessage>();
		msg->CameraId = cameraId;
		msg->IsBinary = true;
		msg->BinaryData = std::move( data );

		{
			std::lock_guard<std::mutex> lock( m_QueueMutex );
			m_OutboundQueue.push( std::move( msg ) );
		}
		m_Condition.notify_one();
	}

	// Send a JSON control message to a single connection (e.g. init on connect)
	void SendControlDirect( crow::websocket::connection* conn, const std::string& json )
	{
		try
		{
			conn->send_text( json );
		}
		catch( ... ) {}
	}

	// Send binary data to a single connection (e.g. init segment on connect)
	void SendBinaryDirect( crow::websocket::connection* conn, Witness::Camera::SegmentBuffer data )
	{
		if( !data || data->empty() ) return;
		try
		{
			conn->send_binary( std::string( (const char*)data->data(), data->size() ) );
		}
		catch( ... ) {}
	}

private:
	struct BroadcastMessage
	{
		int CameraId;
		bool IsBinary;
		std::string TextData;
		Witness::Camera::SegmentBuffer BinaryData;
	};

	void BroadcastLoop()
	{
		while( m_Running )
		{
			std::shared_ptr<BroadcastMessage> msg;
			{
				std::unique_lock<std::mutex> lock( m_QueueMutex );
				m_Condition.wait_for( lock, std::chrono::milliseconds(50),
					[this]{ return !m_OutboundQueue.empty() || !m_Running; } );

				if( !m_Running && m_OutboundQueue.empty() ) break;
				if( m_OutboundQueue.empty() ) continue;

				msg = m_OutboundQueue.front();
				m_OutboundQueue.pop();
			}

			// Get subscribers for this camera
			std::vector<crow::websocket::connection*> conns;
			std::vector<crow::websocket::connection*> toDisconnect;
			{
				std::lock_guard<std::mutex> lock( m_Mutex );
				auto it = m_Subscriptions.find( msg->CameraId );
				if( it == m_Subscriptions.end() ) continue;

				for( auto* conn : it->second )
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

				for( auto* conn : toDisconnect )
				{
					int camId = m_ConnectionCamera[conn];
					m_Subscriptions[camId].erase( conn );
					if( m_Subscriptions[camId].empty() )
						m_Subscriptions.erase( camId );
					m_ConnectionCamera.erase( conn );
					m_PendingCount.erase( conn );
				}
			}

			// Disconnect slow clients (outside lock)
			for( auto* conn : toDisconnect )
			{
				LOG_WARNING( "[MSE] Disconnecting slow client (>%zu pending)", MaxPendingPerClient );
				try { conn->close( "slow consumer" ); } catch( ... ) {}
			}

			// Send to healthy clients
			if( msg->IsBinary && msg->BinaryData && !msg->BinaryData->empty() )
			{
				std::string binaryStr( (const char*)msg->BinaryData->data(), msg->BinaryData->size() );
				for( auto* conn : conns )
				{
					try
					{
						conn->send_binary( binaryStr );
					}
					catch( ... ) {}

					std::lock_guard<std::mutex> lock( m_Mutex );
					auto it = m_PendingCount.find( conn );
					if( it != m_PendingCount.end() && it->second > 0 )
						it->second--;
				}
			}
			else if( !msg->IsBinary )
			{
				for( auto* conn : conns )
				{
					try
					{
						conn->send_text( msg->TextData );
					}
					catch( ... ) {}

					std::lock_guard<std::mutex> lock( m_Mutex );
					auto it = m_PendingCount.find( conn );
					if( it != m_PendingCount.end() && it->second > 0 )
						it->second--;
				}
			}
		}
	}

	// Per-camera subscription tracking
	mutable std::mutex m_Mutex;
	std::map<int, std::set<crow::websocket::connection*>> m_Subscriptions;
	std::map<crow::websocket::connection*, int> m_ConnectionCamera;
	std::map<crow::websocket::connection*, size_t> m_PendingCount;

	// Outbound message queue
	std::mutex m_QueueMutex;
	std::condition_variable m_Condition;
	std::queue<std::shared_ptr<BroadcastMessage>> m_OutboundQueue;

	// Broadcast thread
	std::atomic<bool> m_Running{ false };
	std::thread m_Thread;
};
