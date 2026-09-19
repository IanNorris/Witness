#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Witness::HttpDiagnostics
{
	struct RequestToken
	{
		uint64_t Id = 0;
	};

	struct DurationSummary
	{
		double P50Ms = 0.0;
		double P95Ms = 0.0;
		double P99Ms = 0.0;
		double MaxMs = 0.0;
	};

	struct RouteSnapshot
	{
		std::string Route;
		uint64_t Started = 0;
		uint64_t Completed = 0;
		uint64_t Errors = 0;
		uint64_t Slow = 0;
		double MeanHandlerMs = 0.0;
		double MaxHandlerMs = 0.0;
		double MeanEventLoopMs = 0.0;
		double MaxEventLoopMs = 0.0;
	};

	struct RequestSnapshot
	{
		uint64_t Sequence = 0;
		int64_t StartedUnixMs = 0;
		std::string Method;
		std::string Route;
		std::string Path;
		uint32_t Worker = 0;
		uint64_t Thread = 0;
		int Status = 0;
		uint64_t RequestBytes = 0;
		uint64_t ResponseBytes = 0;
		double HandlerMs = 0.0;
		double EventLoopMs = 0.0;
		double PostHandlerDelayMs = 0.0;
		bool AsyncCompletion = false;
	};

	struct ActiveRequestSnapshot
	{
		uint64_t Sequence = 0;
		int64_t StartedUnixMs = 0;
		std::string Method;
		std::string Route;
		std::string Path;
		uint32_t Worker = 0;
		uint64_t Thread = 0;
		double ElapsedMs = 0.0;
		bool HandlerComplete = false;
		bool WaitingForEventLoop = false;
	};

	struct Snapshot
	{
		uint64_t Started = 0;
		uint64_t Completed = 0;
		uint64_t Errors = 0;
		uint64_t Slow = 0;
		uint64_t InFlight = 0;
		uint64_t MaxInFlight = 0;
		uint32_t ObservedWorkers = 0;
		DurationSummary Handler;
		DurationSummary EventLoop;
		std::vector<RouteSnapshot> Routes;
		std::vector<RequestSnapshot> RecentSlowRequests;
		std::vector<ActiveRequestSnapshot> ActiveRequests;
	};

	RequestToken BeginRequest( const std::string& Method, const std::string& Path,
		const void* EventLoop, size_t RequestBytes );
	void CompleteRequest( RequestToken Token, int Status, size_t ResponseBytes );
	void RecordEventLoopRelease( RequestToken Token );
	void ClearActiveRequests();
	Snapshot GetSnapshot( uint64_t ExcludeActiveRequest = 0 );
}
