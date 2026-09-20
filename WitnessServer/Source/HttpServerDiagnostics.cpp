#include "HttpServerDiagnostics.h"

#include <Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Witness::HttpDiagnostics
{
	namespace
	{
		using SteadyClock = std::chrono::steady_clock;

		constexpr double TraceThresholdMs = 100.0;
		constexpr double SlowThresholdMs = 500.0;
		constexpr size_t MaxSamples = 1024;
		constexpr size_t MaxRecentRequests = 64;
		constexpr size_t MaxTrackedRequests = 4096;
		constexpr size_t MaxTrackedRoutes = 256;
		constexpr size_t MaxSnapshotRoutes = 64;
		constexpr size_t MaxSnapshotActiveRequests = 64;

		struct RouteState
		{
			uint64_t Started = 0;
			uint64_t Completed = 0;
			uint64_t Errors = 0;
			uint64_t Slow = 0;
			uint64_t HandlerSamples = 0;
			uint64_t HandlerTotalUS = 0;
			uint64_t HandlerMaxUS = 0;
			uint64_t EventLoopSamples = 0;
			uint64_t EventLoopTotalUS = 0;
			uint64_t EventLoopMaxUS = 0;
		};

		struct RequestState
		{
			uint64_t Sequence = 0;
			int64_t StartedUnixMs = 0;
			SteadyClock::time_point Started;
			std::string Method;
			std::string Route;
			std::string Path;
			uint32_t Worker = 0;
			uint64_t Thread = 0;
			uint64_t RequestBytes = 0;
			uint64_t ResponseBytes = 0;
			uint64_t HandlerUS = 0;
			uint64_t EventLoopUS = 0;
			int Status = 0;
			bool Completed = false;
			bool EventLoopReleased = false;
		};

		struct State
		{
			std::mutex Mutex;
			uint64_t NextSequence = 1;
			uint32_t NextWorker = 1;
			uint64_t Started = 0;
			uint64_t Completed = 0;
			uint64_t Errors = 0;
			uint64_t Slow = 0;
			uint64_t InFlight = 0;
			uint64_t MaxInFlight = 0;
			std::unordered_map<const void*, uint32_t> Workers;
			std::unordered_map<uint64_t, RequestState> Requests;
			std::unordered_map<std::string, RouteState> Routes;
			std::deque<double> HandlerSamples;
			std::deque<double> EventLoopSamples;
			std::deque<RequestSnapshot> RecentSlowRequests;
		};

		State& GetState()
		{
			static State Instance;
			return Instance;
		}

		uint64_t CurrentThreadId()
		{
			return static_cast<uint64_t>( std::hash<std::thread::id>{}( std::this_thread::get_id() ) );
		}

		int64_t CurrentUnixMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch() ).count();
		}

		uint64_t ElapsedUS( SteadyClock::time_point Started )
		{
			return static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::microseconds>(
				SteadyClock::now() - Started ).count() );
		}

		bool IsVariableSegment( const std::string& Segment )
		{
			if( Segment.empty() ) return false;
			size_t Digits = 0;
			bool Numeric = true;
			for( size_t Index = 0; Index < Segment.size(); ++Index )
			{
				const char Character = Segment[Index];
				if( Character >= '0' && Character <= '9' )
					++Digits;
				else if( !(Index == 0 && Character == '-') )
					Numeric = false;
			}
			return Numeric || Digits >= 8;
		}

		std::string NormalizeRoute( const std::string& Path )
		{
			std::string Result;
			Result.reserve( (std::min)( Path.size(), size_t{ 160 } ) );
			const bool LeadingSlash = !Path.empty() && Path.front() == '/';
			if( LeadingSlash ) Result.push_back( '/' );
			size_t Start = LeadingSlash ? 1 : 0;
			bool FirstSegment = true;
			while( Start < Path.size() && Result.size() < 160 )
			{
				const size_t Slash = Path.find( '/', Start );
				const size_t End = Slash == std::string::npos ? Path.size() : Slash;
				const std::string Segment = Path.substr( Start, End - Start );
				if( !FirstSegment ) Result.push_back( '/' );
				Result += IsVariableSegment( Segment ) ? ":value" : Segment;
				FirstSegment = false;
				if( Slash == std::string::npos ) break;
				Start = Slash + 1;
			}
			if( Result.empty() ) Result = "/";
			if( Result.size() > 160 ) Result.resize( 160 );
			return Result;
		}

		void AddSample( std::deque<double>& Samples, double Value )
		{
			Samples.push_back( Value );
			if( Samples.size() > MaxSamples ) Samples.pop_front();
		}

		std::string RouteKey( State& Diagnostics, const std::string& Method,
			const std::string& Route )
		{
			std::string Key = Method + " " + Route;
			if( Diagnostics.Routes.contains( Key ) ||
				Diagnostics.Routes.size() < MaxTrackedRoutes )
				return Key;
			return "OTHER /:unclassified";
		}

		void MakeRequestTrackingRoom( State& Diagnostics )
		{
			if( Diagnostics.Requests.size() < MaxTrackedRequests ) return;
			auto Oldest = (std::min_element)( Diagnostics.Requests.begin(), Diagnostics.Requests.end(),
				[]( const auto& Left, const auto& Right )
				{
					return Left.second.Started < Right.second.Started;
				} );
			if( Oldest == Diagnostics.Requests.end() ) return;
			if( Diagnostics.InFlight > 0 ) --Diagnostics.InFlight;
			Diagnostics.Requests.erase( Oldest );
		}

		DurationSummary Summarize( const std::deque<double>& Samples )
		{
			DurationSummary Result;
			if( Samples.empty() ) return Result;
			std::vector<double> Sorted( Samples.begin(), Samples.end() );
			std::sort( Sorted.begin(), Sorted.end() );
			auto Percentile = [&]( double Value )
			{
				const size_t Index = (std::min)( Sorted.size() - 1,
					static_cast<size_t>( std::ceil( Value * Sorted.size() ) ) - 1 );
				return Sorted[Index];
			};
			Result.P50Ms = Percentile( 0.50 );
			Result.P95Ms = Percentile( 0.95 );
			Result.P99Ms = Percentile( 0.99 );
			Result.MaxMs = Sorted.back();
			return Result;
		}

		void FinalizeIfReady( State& Diagnostics,
			std::unordered_map<uint64_t, RequestState>::iterator Request )
		{
			if( !Request->second.Completed || !Request->second.EventLoopReleased ) return;
			const auto& Value = Request->second;
			const double HandlerMs = static_cast<double>( Value.HandlerUS ) / 1000.0;
			const double EventLoopMs = static_cast<double>( Value.EventLoopUS ) / 1000.0;
			const double PostHandlerMs = (std::max)( 0.0, EventLoopMs - HandlerMs );
			const bool Slow = HandlerMs >= SlowThresholdMs || EventLoopMs >= SlowThresholdMs;

			AddSample( Diagnostics.HandlerSamples, HandlerMs );
			AddSample( Diagnostics.EventLoopSamples, EventLoopMs );
			if( Slow )
			{
				++Diagnostics.Slow;
				++Diagnostics.Routes[RouteKey( Diagnostics, Value.Method, Value.Route )].Slow;
			}
			if( HandlerMs >= TraceThresholdMs || EventLoopMs >= TraceThresholdMs )
			{
				RequestSnapshot Snapshot;
				Snapshot.Sequence = Value.Sequence;
				Snapshot.StartedUnixMs = Value.StartedUnixMs;
				Snapshot.Method = Value.Method;
				Snapshot.Route = Value.Route;
				Snapshot.Path = Value.Path;
				Snapshot.Worker = Value.Worker;
				Snapshot.Thread = Value.Thread;
				Snapshot.Status = Value.Status;
				Snapshot.RequestBytes = Value.RequestBytes;
				Snapshot.ResponseBytes = Value.ResponseBytes;
				Snapshot.HandlerMs = HandlerMs;
				Snapshot.EventLoopMs = EventLoopMs;
				Snapshot.PostHandlerDelayMs = PostHandlerMs;
				Snapshot.AsyncCompletion = HandlerMs > EventLoopMs + 1.0;
				Diagnostics.RecentSlowRequests.push_back( std::move( Snapshot ) );
				if( Diagnostics.RecentSlowRequests.size() > MaxRecentRequests )
					Diagnostics.RecentSlowRequests.pop_front();
			}
			if( Diagnostics.InFlight > 0 ) --Diagnostics.InFlight;
			Diagnostics.Requests.erase( Request );
		}
	}

	RequestToken BeginRequest( const std::string& Method, const std::string& Path,
		const void* EventLoop, size_t RequestBytes )
	{
		auto& Diagnostics = GetState();
		std::lock_guard Lock( Diagnostics.Mutex );
		MakeRequestTrackingRoom( Diagnostics );
		RequestState Request;
		Request.Sequence = Diagnostics.NextSequence++;
		Request.StartedUnixMs = CurrentUnixMs();
		Request.Started = SteadyClock::now();
		Request.Method = Method;
		Request.Route = NormalizeRoute( Path );
		Request.Path = Path.substr( 0, 256 );
		Request.Thread = CurrentThreadId();
		Request.RequestBytes = static_cast<uint64_t>( RequestBytes );
		auto Worker = Diagnostics.Workers.find( EventLoop );
		if( Worker == Diagnostics.Workers.end() )
			Worker = Diagnostics.Workers.emplace( EventLoop, Diagnostics.NextWorker++ ).first;
		Request.Worker = Worker->second;

		const uint64_t Sequence = Request.Sequence;
		++Diagnostics.Started;
		++Diagnostics.InFlight;
		++Diagnostics.Routes[RouteKey( Diagnostics, Request.Method, Request.Route )].Started;
		Diagnostics.Requests.emplace( Sequence, std::move( Request ) );
		Diagnostics.MaxInFlight = (std::max)( Diagnostics.MaxInFlight, Diagnostics.InFlight );
		return { Sequence };
	}

	void CompleteRequest( RequestToken Token, int Status, size_t ResponseBytes )
	{
		if( Token.Id == 0 ) return;
		auto& Diagnostics = GetState();
		bool LogSlow = false;
		uint64_t Sequence = 0;
		uint32_t Worker = 0;
		std::string Method;
		std::string Path;
		double HandlerMs = 0.0;
		{
			std::lock_guard Lock( Diagnostics.Mutex );
			auto Request = Diagnostics.Requests.find( Token.Id );
			if( Request == Diagnostics.Requests.end() ) return;
			auto& Value = Request->second;
			Value.HandlerUS = ElapsedUS( Value.Started );
			Value.Status = Status;
			Value.ResponseBytes = static_cast<uint64_t>( ResponseBytes );
			Value.Completed = true;
			++Diagnostics.Completed;
			if( Status >= 500 ) ++Diagnostics.Errors;
			auto& Route = Diagnostics.Routes[RouteKey( Diagnostics, Value.Method, Value.Route )];
			++Route.Completed;
			if( Status >= 500 ) ++Route.Errors;
			++Route.HandlerSamples;
			Route.HandlerTotalUS += Value.HandlerUS;
			Route.HandlerMaxUS = (std::max)( Route.HandlerMaxUS, Value.HandlerUS );
			HandlerMs = static_cast<double>( Value.HandlerUS ) / 1000.0;
			LogSlow = HandlerMs >= SlowThresholdMs;
			if( LogSlow )
			{
				Sequence = Value.Sequence;
				Worker = Value.Worker;
				Method = Value.Method;
				Path = Value.Path;
			}
			FinalizeIfReady( Diagnostics, Request );
		}
		if( LogSlow )
		{
			LOG_WARNING( "[HTTP] Slow handler #%llu worker %u: %s %s returned %d in %.1fms",
				static_cast<unsigned long long>( Sequence ), Worker, Method.c_str(), Path.c_str(),
				Status, HandlerMs );
		}
	}

	void RecordEventLoopRelease( RequestToken Token )
	{
		if( Token.Id == 0 ) return;
		auto& Diagnostics = GetState();
		bool LogBlocked = false;
		uint64_t Sequence = 0;
		uint32_t Worker = 0;
		std::string Method;
		std::string Path;
		double EventLoopMs = 0.0;
		double HandlerMs = 0.0;
		{
			std::lock_guard Lock( Diagnostics.Mutex );
			auto Request = Diagnostics.Requests.find( Token.Id );
			if( Request == Diagnostics.Requests.end() ) return;
			auto& Value = Request->second;
			Value.EventLoopUS = ElapsedUS( Value.Started );
			Value.EventLoopReleased = true;
			auto& Route = Diagnostics.Routes[RouteKey( Diagnostics, Value.Method, Value.Route )];
			++Route.EventLoopSamples;
			Route.EventLoopTotalUS += Value.EventLoopUS;
			Route.EventLoopMaxUS = (std::max)( Route.EventLoopMaxUS, Value.EventLoopUS );
			EventLoopMs = static_cast<double>( Value.EventLoopUS ) / 1000.0;
			HandlerMs = static_cast<double>( Value.HandlerUS ) / 1000.0;
			LogBlocked = Value.Completed && EventLoopMs >= SlowThresholdMs &&
				EventLoopMs - HandlerMs >= 250.0;
			if( LogBlocked )
			{
				Sequence = Value.Sequence;
				Worker = Value.Worker;
				Method = Value.Method;
				Path = Value.Path;
			}
			FinalizeIfReady( Diagnostics, Request );
		}
		if( LogBlocked )
		{
			LOG_WARNING( "[HTTP] Worker %u unavailable after #%llu %s %s for %.1fms (handler %.1fms)",
				Worker, static_cast<unsigned long long>( Sequence ), Method.c_str(), Path.c_str(),
				EventLoopMs, HandlerMs );
		}
	}

	void ClearActiveRequests()
	{
		auto& Diagnostics = GetState();
		std::lock_guard Lock( Diagnostics.Mutex );
		Diagnostics.Requests.clear();
		Diagnostics.InFlight = 0;
	}

	Snapshot GetSnapshot( uint64_t ExcludeActiveRequest )
	{
		auto& Diagnostics = GetState();
		std::lock_guard Lock( Diagnostics.Mutex );
		Snapshot Result;
		Result.Started = Diagnostics.Started;
		Result.Completed = Diagnostics.Completed;
		Result.Errors = Diagnostics.Errors;
		Result.Slow = Diagnostics.Slow;
		Result.InFlight = static_cast<uint64_t>( std::count_if(
			Diagnostics.Requests.begin(), Diagnostics.Requests.end(),
			[ExcludeActiveRequest]( const auto& Item )
			{
				return Item.first != ExcludeActiveRequest;
			} ) );
		Result.MaxInFlight = Diagnostics.MaxInFlight;
		Result.ObservedWorkers = static_cast<uint32_t>( Diagnostics.Workers.size() );
		Result.Handler = Summarize( Diagnostics.HandlerSamples );
		Result.EventLoop = Summarize( Diagnostics.EventLoopSamples );

		Result.Routes.reserve( Diagnostics.Routes.size() );
		for( const auto& [Name, Value] : Diagnostics.Routes )
		{
			RouteSnapshot Route;
			Route.Route = Name;
			Route.Started = Value.Started;
			Route.Completed = Value.Completed;
			Route.Errors = Value.Errors;
			Route.Slow = Value.Slow;
			Route.MeanHandlerMs = Value.HandlerSamples ?
				static_cast<double>( Value.HandlerTotalUS ) / (1000.0 * Value.HandlerSamples) : 0.0;
			Route.MaxHandlerMs = static_cast<double>( Value.HandlerMaxUS ) / 1000.0;
			Route.MeanEventLoopMs = Value.EventLoopSamples ?
				static_cast<double>( Value.EventLoopTotalUS ) / (1000.0 * Value.EventLoopSamples) : 0.0;
			Route.MaxEventLoopMs = static_cast<double>( Value.EventLoopMaxUS ) / 1000.0;
			Result.Routes.push_back( std::move( Route ) );
		}
		std::sort( Result.Routes.begin(), Result.Routes.end(),
			[]( const auto& Left, const auto& Right ) { return Left.Started > Right.Started; } );
		if( Result.Routes.size() > MaxSnapshotRoutes ) Result.Routes.resize( MaxSnapshotRoutes );

		Result.RecentSlowRequests.assign(
			Diagnostics.RecentSlowRequests.rbegin(), Diagnostics.RecentSlowRequests.rend() );
		const auto Now = SteadyClock::now();
		for( const auto& [Sequence, Value] : Diagnostics.Requests )
		{
			if( Sequence == ExcludeActiveRequest ) continue;
			ActiveRequestSnapshot Active;
			Active.Sequence = Sequence;
			Active.StartedUnixMs = Value.StartedUnixMs;
			Active.Method = Value.Method;
			Active.Route = Value.Route;
			Active.Path = Value.Path;
			Active.Worker = Value.Worker;
			Active.Thread = Value.Thread;
			Active.ElapsedMs = static_cast<double>( std::chrono::duration_cast<std::chrono::microseconds>(
				Now - Value.Started ).count() ) / 1000.0;
			Active.HandlerComplete = Value.Completed;
			Active.WaitingForEventLoop = Value.Completed && !Value.EventLoopReleased;
			Result.ActiveRequests.push_back( std::move( Active ) );
		}
		std::sort( Result.ActiveRequests.begin(), Result.ActiveRequests.end(),
			[]( const auto& Left, const auto& Right ) { return Left.ElapsedMs > Right.ElapsedMs; } );
		if( Result.ActiveRequests.size() > MaxSnapshotActiveRequests )
			Result.ActiveRequests.resize( MaxSnapshotActiveRequests );
		return Result;
	}
}
