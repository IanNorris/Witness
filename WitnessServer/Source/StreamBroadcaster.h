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
#include <functional>
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
	static constexpr size_t MaxPendingPerClient = 128; // Higher than EventBroadcaster -- binary data is larger
	static constexpr int SubStreamChannelOffset = 10000;
	struct BootstrapPayload
	{
		std::string ControlJson;
		Witness::Camera::SegmentBuffer Data;
	};

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
		// A slow WebSocket send may keep m_Mutex held until the network stack
		// accepts it. Camera ingest must never wait behind that send: returning
		// true merely queues a message which the broadcaster can discard later
		// if the camera no longer has viewers.
		std::unique_lock<std::mutex> lock( m_Mutex, std::try_to_lock );
		if( !lock.owns_lock() )
			return true;
		auto it = m_Subscriptions.find( cameraId );
		return it != m_Subscriptions.end() && !it->second.empty();
	}

	void Resubscribe( int cameraId, crow::websocket::connection* conn )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		auto existing = m_ConnectionCamera.find( conn );
		if( existing != m_ConnectionCamera.end() )
		{
			const int oldCameraId = existing->second;
			if( oldCameraId == cameraId )
				return;
			auto subscribers = m_Subscriptions.find( oldCameraId );
			if( subscribers != m_Subscriptions.end() )
			{
				subscribers->second.erase( conn );
				if( subscribers->second.empty() )
					m_Subscriptions.erase( subscribers );
			}
		}

		m_Subscriptions[cameraId].insert( conn );
		m_ConnectionCamera[conn] = cameraId;
		m_PendingCount[conn] = 0;
		LOG_INFO( "[MSE] Client switched to stream channel %d (%d viewers)",
			cameraId, (int)m_Subscriptions[cameraId].size() );
	}

	// Atomically move a connection to a channel and bootstrap its decoder. The
	// broadcast loop uses the same lock, so no queued control/binary pair can
	// split selection, init metadata, and init bytes during a stream switch.
	void SubscribeWithBootstrap(
		int cameraId, crow::websocket::connection* conn,
		const std::string& selectionJson,
		const std::function<BootstrapPayload()>& buildBootstrap )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		try
		{
			conn->send_text( selectionJson );
		}
		catch( ... ) {}

		auto existing = m_ConnectionCamera.find( conn );
		if( existing != m_ConnectionCamera.end() )
		{
			const int oldCameraId = existing->second;
			auto subscribers = m_Subscriptions.find( oldCameraId );
			if( subscribers != m_Subscriptions.end() )
			{
				subscribers->second.erase( conn );
				if( subscribers->second.empty() )
					m_Subscriptions.erase( subscribers );
			}
		}

		m_Subscriptions[cameraId].insert( conn );
		m_ConnectionCamera[conn] = cameraId;
		m_PendingCount[conn] = 0;

		// Capture the init only after the subscription is visible. HasViewers()
		// deliberately returns true while this lock is held, so a concurrent
		// reconnect queues its replacement init instead of dropping it.
		const auto bootstrap = buildBootstrap();
		if( !bootstrap.ControlJson.empty() && bootstrap.Data && !bootstrap.Data->empty() )
		{
			try
			{
				conn->send_text( bootstrap.ControlJson );
				conn->send_binary( std::string(
					reinterpret_cast<const char*>( bootstrap.Data->data() ), bootstrap.Data->size() ) );
			}
			catch( ... ) {}
		}

		LOG_INFO( "[MSE] Client subscribed to stream channel %d (%d viewers)",
			cameraId, (int)m_Subscriptions[cameraId].size() );
	}

	int GetSubscriptionChannel( crow::websocket::connection* conn ) const
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		auto it = m_ConnectionCamera.find( conn );
		return it != m_ConnectionCamera.end() ? it->second : 0;
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
	// Must only be called for connections that are subscribed.
	void SendControlDirect( crow::websocket::connection* conn, const std::string& json )
	{
		std::lock_guard<std::mutex> lock( m_Mutex );
		if( m_ConnectionCamera.find( conn ) == m_ConnectionCamera.end() )
			return;
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
		std::lock_guard<std::mutex> lock( m_Mutex );
		if( m_ConnectionCamera.find( conn ) == m_ConnectionCamera.end() )
			return;
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
				m_Condition.wait_for( lock, std::chrono::milliseconds(5),
					[this]{ return !m_OutboundQueue.empty() || !m_Running; } );

				if( !m_Running && m_OutboundQueue.empty() ) break;
				if( m_OutboundQueue.empty() ) continue;

				msg = m_OutboundQueue.front();
				m_OutboundQueue.pop();
			}

			// Prepare the binary string once outside the lock
			std::string binaryStr;
			if( msg->IsBinary && msg->BinaryData && !msg->BinaryData->empty() )
			{
				binaryStr.assign( (const char*)msg->BinaryData->data(), msg->BinaryData->size() );
			}

			// Hold the lock for the entire broadcast to prevent use-after-free
			// when connections close concurrently. Subscribe/Unsubscribe will
			// block briefly during sends, which is acceptable for correctness.
			std::lock_guard<std::mutex> lock( m_Mutex );

			auto subIt = m_Subscriptions.find( msg->CameraId );
			if( subIt == m_Subscriptions.end() ) continue;

			// Copy the connection set -- we may modify m_Subscriptions during iteration
			std::vector<crow::websocket::connection*> conns( subIt->second.begin(), subIt->second.end() );

			for( auto* conn : conns )
			{
				// Verify connection is still subscribed (may have been removed by
				// concurrent Unsubscribe or slow-client disconnect)
				if( m_ConnectionCamera.find( conn ) == m_ConnectionCamera.end() )
					continue;

				// Check for slow client
				auto& pending = m_PendingCount[conn];
				if( pending >= MaxPendingPerClient )
				{
					// Remove from all maps before closing
					int camId = m_ConnectionCamera[conn];
					m_Subscriptions[camId].erase( conn );
					if( m_Subscriptions[camId].empty() )
						m_Subscriptions.erase( camId );
					m_ConnectionCamera.erase( conn );
					m_PendingCount.erase( conn );

					LOG_WARNING( "[MSE] Disconnecting slow client (>%zu pending)", MaxPendingPerClient );
					try { conn->close( "slow consumer" ); } catch( ... ) {}
					continue;
				}

				pending++;

				// Send data -- catch exceptions in case connection closed
				// between our check and the send
				try
				{
					if( msg->IsBinary )
						conn->send_binary( binaryStr );
					else
						conn->send_text( msg->TextData );
				}
				catch( ... ) {}

				// Re-verify connection still exists before decrementing
				auto pendIt = m_PendingCount.find( conn );
				if( pendIt != m_PendingCount.end() && pendIt->second > 0 )
					pendIt->second--;
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
