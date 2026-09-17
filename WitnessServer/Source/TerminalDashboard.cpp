#include "TerminalDashboard.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
	const auto DashboardStarted = std::chrono::steady_clock::now();

	const char* LevelName( Witness::LogLevel Level )
	{
		switch( Level )
		{
			case Witness::LogLevel::Debug: return "DBG";
			case Witness::LogLevel::Info: return "INF";
			case Witness::LogLevel::Warning: return "WRN";
			case Witness::LogLevel::Error: return "ERR";
			default: return "---";
		}
	}

	std::string CompactDuration( std::chrono::seconds Value )
	{
		auto Hours = std::chrono::duration_cast<std::chrono::hours>( Value );
		Value -= Hours;
		auto Minutes = std::chrono::duration_cast<std::chrono::minutes>( Value );
		Value -= Minutes;
		return std::format( "{:02}:{:02}:{:02}", Hours.count(), Minutes.count(), Value.count() );
	}

	std::string Fit( std::string Value, size_t Width )
	{
		if( Value.size() > Width )
		{
			if( Width > 1 )
				Value = Value.substr( 0, Width - 1 ) + "~";
			else
				Value.resize( Width );
		}
		else
			Value.append( Width - Value.size(), ' ' );
		return Value;
	}
}

TerminalDashboard::TerminalDashboard( Witness::LogLevel minimumLevel )
	: m_MinimumLevel( minimumLevel )
{
}

TerminalDashboard::~TerminalDashboard()
{
	Stop();
}

bool TerminalDashboard::IsSupported()
{
#ifdef _WIN32
	HANDLE Output = GetStdHandle( STD_OUTPUT_HANDLE );
	DWORD Mode = 0;
	if( Output == INVALID_HANDLE_VALUE || GetFileType( Output ) != FILE_TYPE_CHAR ||
		!GetConsoleMode( Output, &Mode ) )
		return false;
	if( !SetConsoleMode( Output, Mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING ) )
		return false;
	SetConsoleMode( Output, Mode );
	return true;
#else
	return false;
#endif
}

void TerminalDashboard::Start( SnapshotProvider provider )
{
	if( m_Running.exchange( true ) )
		return;
	m_Provider = std::move( provider );
	Witness::LogSetObserver( &TerminalDashboard::ObserveLog, this );
	m_Thread = std::thread( &TerminalDashboard::Run, this );
}

void TerminalDashboard::Stop()
{
	m_Running = false;
	Witness::LogSetObserver( nullptr, nullptr );
	if( m_Thread.joinable() )
		m_Thread.join();
}

void TerminalDashboard::ObserveLog( void* context, Witness::LogLevel level,
	const char* timestamp, const char* message )
{
	static_cast<TerminalDashboard*>( context )->AddLog( level, timestamp, message );
}

int TerminalDashboard::AttributeCamera( const std::string& message ) const
{
	auto ParseAfter = [&]( const char* Prefix ) -> int
	{
		auto Position = message.find( Prefix );
		if( Position == std::string::npos ) return -1;
		Position += strlen( Prefix );
		if( Position >= message.size() || message[Position] < '0' || message[Position] > '9' )
			return -1;
		int Value = 0;
		while( Position < message.size() && message[Position] >= '0' && message[Position] <= '9' )
			Value = Value * 10 + ( message[Position++] - '0' );
		return Value;
	};
	int Explicit = ParseAfter( "Camera " );
	if( Explicit < 0 ) Explicit = ParseAfter( "camera " );
	if( Explicit < 0 ) Explicit = ParseAfter( "source " );
	if( Explicit >= 0 ) return Explicit;

	std::lock_guard<std::mutex> lock( m_Mutex );
	for( const auto& Camera : m_LastStatus.Cameras )
	{
		const std::string CameraToken = "Camera " + std::to_string( Camera.Id );
		const std::string SourceToken = "source " + std::to_string( Camera.Id );
		if( message.find( CameraToken ) != std::string::npos ||
			message.find( SourceToken ) != std::string::npos ||
			( !Camera.Name.empty() && message.starts_with( Camera.Name + ":" ) ) )
			return Camera.Id;
	}
	return -1;
}

void TerminalDashboard::AddLog( Witness::LogLevel level, const char* timestamp, const char* message )
{
	if( level < m_MinimumLevel )
		return;
	Entry NewEntry;
	NewEntry.Level = level;
	NewEntry.Timestamp = timestamp ? timestamp : "";
	NewEntry.Message = message ? message : "";
	NewEntry.CameraId = AttributeCamera( NewEntry.Message );

	std::lock_guard<std::mutex> lock( m_Mutex );
	m_Entries.push_back( std::move( NewEntry ) );
	while( m_Entries.size() > 400 )
		m_Entries.pop_front();
}

void TerminalDashboard::Run() noexcept
{
#ifdef _WIN32
	HANDLE Output = GetStdHandle( STD_OUTPUT_HANDLE );
	DWORD OriginalMode = 0;
	if( !GetConsoleMode( Output, &OriginalMode ) ||
		!SetConsoleMode( Output, OriginalMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING ) )
	{
		m_Running = false;
		Witness::LogSetConsoleLevel( Witness::LogLevel::Warning );
		return;
	}
	struct ConsoleRestore
	{
		HANDLE Output;
		DWORD Mode;
		~ConsoleRestore()
		{
			DWORD Written = 0;
			const char* Leave = "\x1b[?25h\x1b[?1049l";
			WriteConsoleA( Output, Leave, (DWORD)strlen( Leave ), &Written, nullptr );
			SetConsoleMode( Output, Mode );
		}
	} Restore{ Output, OriginalMode };

	const char* Enter = "\x1b[?1049h\x1b[?25l\x1b[2J";
	DWORD Written = 0;
	if( !WriteConsoleA( Output, Enter, (DWORD)strlen( Enter ), &Written, nullptr ) )
	{
		m_Running = false;
		Witness::LogSetConsoleLevel( Witness::LogLevel::Warning );
		return;
	}

	try
	{
		bool UpWasDown = false;
		bool DownWasDown = false;
		bool AllWasDown = false;
		while( m_Running.load() )
		{
		OperationalStatus Snapshot;
		try
		{
			if( m_Provider )
				Snapshot = m_Provider();
		}
		catch( ... )
		{
			// Rendering is diagnostic-only and must never take down the server.
		}

		{
			std::lock_guard<std::mutex> lock( m_Mutex );
			m_LastStatus = Snapshot;
		}

		bool Up = ( GetAsyncKeyState( VK_UP ) & 0x8000 ) != 0;
		bool Down = ( GetAsyncKeyState( VK_DOWN ) & 0x8000 ) != 0;
		bool All = ( GetAsyncKeyState( 'A' ) & 0x8000 ) != 0;
		if( All && !AllWasDown )
			m_SelectedCamera = -1;
		if( !Snapshot.Cameras.empty() )
		{
			auto Current = std::find_if( Snapshot.Cameras.begin(), Snapshot.Cameras.end(),
				[&]( const auto& Camera ) { return Camera.Id == m_SelectedCamera; } );
			int Index = Current == Snapshot.Cameras.end() ? -1 :
				(int)std::distance( Snapshot.Cameras.begin(), Current );
			if( Down && !DownWasDown )
				m_SelectedCamera = Snapshot.Cameras[( Index + 1 ) % Snapshot.Cameras.size()].Id;
			if( Up && !UpWasDown )
			{
				Index = Index < 0 ? 0 : Index;
				m_SelectedCamera = Snapshot.Cameras[( Index + (int)Snapshot.Cameras.size() - 1 ) %
					Snapshot.Cameras.size()].Id;
			}
		}
		UpWasDown = Up;
		DownWasDown = Down;
		AllWasDown = All;

		CONSOLE_SCREEN_BUFFER_INFO Info{};
		GetConsoleScreenBufferInfo( Output, &Info );
		// Leave the final column untouched: writing into it can wrap and scroll the
		// alternate buffer on some Windows terminal hosts.
		const int Width = std::max<int>( 1, Info.srWindow.Right - Info.srWindow.Left );
		const int Height = std::max<int>( 1, Info.srWindow.Bottom - Info.srWindow.Top + 1 );

		int Connected = 0;
		uint64_t Essential = 0, AI = 0;
		double OldestEssential = 0.0, OldestAI = 0.0;
		for( const auto& Camera : Snapshot.Cameras )
		{
			Connected += Camera.State == "Connected";
			Essential += Camera.PendingEssential;
			AI += Camera.PendingAI;
			OldestEssential = std::max( OldestEssential, Camera.OldestEssentialMs );
			OldestAI = std::max( OldestAI, Camera.OldestAIMs );
		}

		const double UsedMemoryGiB = (double)( Snapshot.HostTotalMemoryBytes -
			std::min( Snapshot.HostAvailableMemoryBytes, Snapshot.HostTotalMemoryBytes ) ) /
			( 1024.0 * 1024.0 * 1024.0 );
		const double TotalMemoryGiB = (double)Snapshot.HostTotalMemoryBytes /
			( 1024.0 * 1024.0 * 1024.0 );
		const double PrivateMemoryGiB = (double)Snapshot.ProcessPrivateBytes /
			( 1024.0 * 1024.0 * 1024.0 );
		std::vector<std::string> Lines;
		Lines.push_back( std::format( "Witness  build {}  up {}  cameras {}/{}  CPU {}  RAM {:.1f}/{:.1f}G  proc {:.1f}G  port {}",
			Snapshot.BuildHash.empty() ? "starting" : Snapshot.BuildHash,
			CompactDuration( std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - DashboardStarted ) ), Connected,
			Snapshot.Cameras.size(), Snapshot.HostCpuPercent < 0.0 ? "--" :
				std::format( "{:.0f}%", Snapshot.HostCpuPercent ), UsedMemoryGiB,
			TotalMemoryGiB, PrivateMemoryGiB, Snapshot.Port ) );
		Lines.push_back( std::format( "Queues  essential {} (oldest {:.0f}ms)  AI {} (oldest {:.0f}ms)    Up/Down select  A all logs  Ctrl+C stop",
			Essential, OldestEssential, AI, OldestAI ) );
		Lines.push_back( std::string( Width, '-' ) );
		Lines.push_back( "   ID Camera             State         Stream             Flags Seg Rst Drop Cor* Repair E/A" );

		const int ReservedLogLines = std::min( 10, std::max( 4, Height / 3 ) );
		const int MaxCameras = std::max( 1, Height - ReservedLogLines - 6 );
		int SelectedIndex = -1;
		for( int Index = 0; Index < (int)Snapshot.Cameras.size(); ++Index )
			if( Snapshot.Cameras[Index].Id == m_SelectedCamera ) SelectedIndex = Index;
		const int CameraStart = SelectedIndex >= MaxCameras ? SelectedIndex - MaxCameras + 1 : 0;
		for( int Index = CameraStart; Index < (int)Snapshot.Cameras.size() &&
			Index < CameraStart + MaxCameras; ++Index )
		{
			const auto& Camera = Snapshot.Cameras[Index];
			std::string Stream = Camera.MainAvailable ?
				std::format( "{} {}x{}{}", Camera.Codec.empty() ? "?" : Camera.Codec,
					Camera.Width, Camera.Height, Camera.MainEstablished ? "" : " init" ) : "unavailable";
			std::string Flags;
			if( Camera.Recording ) Flags += "R";
			if( Camera.MotionActive ) Flags += "M";
			if( Camera.PreviewConnected ) Flags += "P";
			Lines.push_back( std::format( "{} {:>2} {} {} {} {} {:>3} {:>3} {:>4} {:>4} {:>6} {}/{}",
				Camera.Id == m_SelectedCamera ? '>' : ' ', Camera.Id, Fit( Camera.Name, 18 ),
				Fit( Camera.State, 13 ), Fit( Stream, 18 ), Fit( Flags, 5 ),
				Camera.RetainedSegments, Camera.Reconnects, Camera.DroppedPackets, Camera.CorruptPackets,
				Camera.RepairedTimestamps, Camera.PendingEssential, Camera.PendingAI ) );
		}

		Lines.push_back( std::string( Width, '-' ) );
		std::string LogTitle = m_SelectedCamera < 0 ? "Recent server events" :
			std::format( "Recent events for camera {} (unattributed warnings also shown)", m_SelectedCamera );
		Lines.push_back( LogTitle );
		std::vector<Entry> Visible;
		{
			std::lock_guard<std::mutex> lock( m_Mutex );
			for( auto It = m_Entries.rbegin(); It != m_Entries.rend() &&
				(int)Visible.size() < ReservedLogLines; ++It )
			{
				if( m_SelectedCamera < 0 || It->CameraId == m_SelectedCamera ||
					( It->CameraId < 0 && It->Level >= Witness::LogLevel::Warning ) )
					Visible.push_back( *It );
			}
		}
		std::reverse( Visible.begin(), Visible.end() );
		for( const auto& Entry : Visible )
		{
			std::string Time = Entry.Timestamp.size() >= 23 ? Entry.Timestamp.substr( 11, 12 ) : Entry.Timestamp;
			Lines.push_back( std::format( "{} {} {}", Time, LevelName( Entry.Level ), Entry.Message ) );
		}

		std::string Screen = "\x1b[H";
		for( int Row = 0; Row < Height; ++Row )
		{
			std::string Line = Row < (int)Lines.size() ? Lines[Row] : "";
			if( Line.size() > (size_t)Width ) Line.resize( Width );
			else Line.append( Width - Line.size(), ' ' );
			Screen += Line;
			if( Row + 1 < Height ) Screen += '\n';
		}
		WriteConsoleA( Output, Screen.data(), (DWORD)Screen.size(), &Written, nullptr );
		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
		}
	}
	catch( ... )
	{
		m_Running = false;
		Witness::LogSetConsoleLevel( Witness::LogLevel::Warning );
	}
#endif
}
