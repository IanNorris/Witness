#pragma once

#include "OperationalStatus.h"
#include <Log.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class TerminalDashboard
{
public:
	using SnapshotProvider = std::function<OperationalStatus()>;

	explicit TerminalDashboard( Witness::LogLevel minimumLevel );
	~TerminalDashboard();

	void Start( SnapshotProvider provider );
	void Stop();

	// Future standalone presenters consume the same model through IPC. The
	// in-process presenter uses a callback so it has no authentication or port
	// dependency during server startup.
	static bool IsSupported();

private:
	struct Entry
	{
		Witness::LogLevel Level = Witness::LogLevel::Info;
		std::string Timestamp;
		std::string Message;
		int CameraId = -1;
	};

	static void ObserveLog( void* context, Witness::LogLevel level,
		const char* timestamp, const char* message );
	void AddLog( Witness::LogLevel level, const char* timestamp, const char* message );
	void Run() noexcept;
	int AttributeCamera( const std::string& message ) const;

	Witness::LogLevel m_MinimumLevel;
	std::atomic<bool> m_Running{ false };
	std::thread m_Thread;
	SnapshotProvider m_Provider;
	mutable std::mutex m_Mutex;
	std::deque<Entry> m_Entries;
	OperationalStatus m_LastStatus;
	int m_SelectedCamera = -1;
};
