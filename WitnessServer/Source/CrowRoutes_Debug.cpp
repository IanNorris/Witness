#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "CameraWorker.h"
#include "SQLite.h"
#include "ClipReprocessWorker.h"

#include <Log.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <windows.h>
#include <psapi.h>

#ifdef CROW_ENABLE_SSL
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#endif
// ===== Debug Handlers =====

namespace
{
	const auto HealthProcessStart = std::chrono::steady_clock::now();
	std::atomic<uint64_t> HealthSampleSequence{ 0 };
}

void CrowListener::HandleDebugEnum( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::vector<crow::json::wvalue> Array;

	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		crow::json::wvalue ValueOut;
		ValueOut["name"] = Value->GetName();
		ValueOut["value"] = Value->Get();
		Array.push_back( std::move( ValueOut ) );
	}

	crow::json::wvalue Data;
	Data["values"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugSet( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("name") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string Name = body["name"].s();
	std::string ValueIn = body["value"].s();

	bool Success = false;
	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		if( Name == Value->GetName() )
		{
			Success = Value->Set( ValueIn.c_str() );
			break;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Success ? 200 : 400;
	res.end();
}

void CrowListener::HandleDebugReset( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("name") )
	{
		res.code = 400;
		res.body = "Missing name field";
		res.end();
		return;
	}

	std::string Name = body["name"].s();

	bool Success = false;
	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		if( Name == Value->GetName() )
		{
			Value->Reset();
			Success = true;
			break;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Success ? 200 : 400;
	res.end();
}

void CrowListener::HandleDebugReloadTLS( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	bool Success = ReloadTLS();

	crow::json::wvalue Data;
	Data["success"] = Success;
	Data["message"] = Success ? "TLS certificate reloaded" : "TLS reload failed -- check server logs";

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = Success ? 200 : 500;
	res.end();
}

void CrowListener::HandleDebugHealth( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}
	const auto CollectionStart = std::chrono::steady_clock::now();

	struct CameraHealthSnapshot
	{
		int Id = 0;
		std::string Name;
		std::string Status;
		std::shared_ptr<CameraWorker> Worker;
	};

	std::string BuildHash;
	std::vector<CameraHealthSnapshot> CameraSnapshots;
	{
		std::shared_lock<std::shared_mutex> Lock( m_GlobalContext->Mutex );
		BuildHash = m_GlobalContext->BuildHash;
		CameraSnapshots.reserve( m_GlobalContext->GetCameraMap().size() );
		for( const auto& [Id, State] : m_GlobalContext->GetCameraMap() )
		{
			CameraSnapshots.push_back( { Id, State.Name, State.Status, State.Worker } );
		}
	}

	crow::json::wvalue Host;
	SYSTEM_INFO SystemInfo{};
	GetSystemInfo( &SystemInfo );
	const DWORD ActiveProcessors = GetActiveProcessorCount( ALL_PROCESSOR_GROUPS );
	Host["logicalProcessors"] = (uint64_t)(ActiveProcessors ? ActiveProcessors :
		SystemInfo.dwNumberOfProcessors);

	MEMORYSTATUSEX Memory{};
	Memory.dwLength = sizeof( Memory );
	if( GlobalMemoryStatusEx( &Memory ) )
	{
		Host["memoryCollectionAvailable"] = true;
		Host["totalMemoryBytes"] = (uint64_t)Memory.ullTotalPhys;
		Host["availableMemoryBytes"] = (uint64_t)Memory.ullAvailPhys;
		Host["memoryLoadPercent"] = (uint64_t)Memory.dwMemoryLoad;
	}
	else
	{
		Host["memoryCollectionAvailable"] = false;
		Host["totalMemoryBytes"] = nullptr;
		Host["availableMemoryBytes"] = nullptr;
		Host["memoryLoadPercent"] = nullptr;
	}

	PROCESS_MEMORY_COUNTERS_EX ProcessMemory{};
	if( GetProcessMemoryInfo( GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &ProcessMemory ), sizeof( ProcessMemory ) ) )
	{
		Host["processMemoryCollectionAvailable"] = true;
		Host["processWorkingSetBytes"] = (uint64_t)ProcessMemory.WorkingSetSize;
		Host["processPrivateBytes"] = (uint64_t)ProcessMemory.PrivateUsage;
	}
	else
	{
		Host["processMemoryCollectionAvailable"] = false;
		Host["processWorkingSetBytes"] = nullptr;
		Host["processPrivateBytes"] = nullptr;
	}

	auto MeanQueueMS = []( int64_t TotalNS, uint64_t Samples )
	{
		return Samples ? (double)TotalNS / ((double)Samples * 1000000.0) : 0.0;
	};
	auto StreamJson = []( const char* Tier, bool Connected,
		const std::shared_ptr<Witness::Camera::LiveOutputStream>& Stream )
	{
		crow::json::wvalue Value;
		Value["tier"] = Tier;
		Value["available"] = Stream != nullptr;
		Value["state"] = Connected ? "connected" : (Stream ? "disconnected" : "unavailable");
		if( !Stream )
			return Value;

		auto Diag = Stream->GetStreamingDiagnostics( false );
		Value["initGeneration"] = Diag.InitGeneration;
		Value["segmentIndex"] = Diag.CurrentSegmentIndex;
		Value["retainedSegments"] = Diag.BacklogSize;
		Value["videoCodec"] = Diag.VideoCodec;
		Value["audioCodec"] = Diag.AudioCodec;
		Value["inputFormat"] = Diag.InputFormat;
		Value["width"] = Diag.VideoWidth;
		Value["height"] = Diag.VideoHeight;
		Value["acceptedVideoPackets"] = (uint64_t)Diag.AcceptedVideoPackets;
		Value["acceptedVideoKeyframes"] = (uint64_t)Diag.AcceptedVideoKeyframes;
		Value["repairedVideoTimestamps"] = (uint64_t)Diag.RepairedVideoTimestamps;
		Value["droppedVideoPackets"] = (uint64_t)Diag.DroppedVideoPackets;
		Value["missingVideoDtsPackets"] = (uint64_t)Diag.MissingVideoDtsPackets;
		Value["corruptVideoPackets"] = (uint64_t)Diag.CorruptVideoPackets;
		Value["streamEstablished"] = Diag.StreamEstablished;
		Value["startupGraceElapsedMs"] = Diag.StartupGraceElapsedMs;
		Value["startupAcceptedVideoPackets"] = (uint64_t)Diag.StartupAcceptedVideoPackets;
		Value["startupDroppedVideoPackets"] = (uint64_t)Diag.StartupDroppedVideoPackets;
		Value["startupRepairedVideoTimestamps"] = (uint64_t)Diag.StartupRepairedVideoTimestamps;
		Value["establishedAcceptedVideoPackets"] = (uint64_t)Diag.EstablishedAcceptedVideoPackets;
		Value["establishedDroppedVideoPackets"] = (uint64_t)Diag.EstablishedDroppedVideoPackets;
		Value["establishedRepairedVideoTimestamps"] = (uint64_t)Diag.EstablishedRepairedVideoTimestamps;
		crow::json::wvalue DropReasons;
		DropReasons["waitingForKeyframe"] = (uint64_t)Diag.WaitingForKeyframePackets;
		DropReasons["missingTimestamp"] = (uint64_t)Diag.MissingTimestampPackets;
		DropReasons["beforeVideoEpoch"] = (uint64_t)Diag.BeforeVideoEpochPackets;
		DropReasons["negativeTimestamp"] = (uint64_t)Diag.NegativeTimestampPackets;
		DropReasons["nonMonotonicInput"] = (uint64_t)Diag.NonMonotonicInputPackets;
		DropReasons["noMuxBuffer"] = (uint64_t)Diag.NoMuxBufferPackets;
		DropReasons["nonMonotonicOutput"] = (uint64_t)Diag.NonMonotonicOutputPackets;
		DropReasons["muxError"] = (uint64_t)Diag.MuxErrorPackets;
		DropReasons["decodeCorruption"] = (uint64_t)Diag.DecodeCorruptionEvents;
		DropReasons["decodeRecovery"] = (uint64_t)Diag.DecodeRecoveryEvents;
		Value["packetDispositionCounts"] = std::move( DropReasons );
		Value["timestampCorrectionSaturatedPackets"] =
			(uint64_t)Diag.TimestampCorrectionSaturatedPackets;
		Value["timestampNormalizationLastCompletedSegment"] = Diag.TotalSegments > 0 ?
			crow::json::wvalue( Diag.TimestampNormalizationActive ) : crow::json::wvalue( nullptr );
		Value["timestampNormalizationProvenance"] =
			"lastCompletedSegmentMayPrecedeReconnect";
		Value["videoPhaseErrorMs"] = Diag.VideoPhaseErrorMs;
		Value["videoCorrectionMs"] = Diag.VideoCorrectionMs;
		Value["audioVideoSkewMs"] = Diag.HasAudioVideoSkew ?
			crow::json::wvalue( Diag.AudioVideoSkewMs ) : crow::json::wvalue( nullptr );
		Value["audioVideoSkewAvailable"] = Diag.HasAudioVideoSkew;
		Value["totalSegments"] = Diag.TotalSegments;
		Value["maxSegmentDriftMs"] = Diag.MaxDriftMs;
		Value["initStructureObserved"] = Diag.InitStructureObserved;
		Value["initStructureValid"] = Diag.InitStructureObserved ?
			crow::json::wvalue( Diag.InitStructureValid ) : crow::json::wvalue( nullptr );
		Value["initStructureError"] = Diag.InitStructureObserved ?
			crow::json::wvalue( Diag.InitStructureError ) : crow::json::wvalue( nullptr );
		std::vector<crow::json::wvalue> MediaEvents;
		MediaEvents.reserve( Diag.RecentMediaEvents.size() );
		for( const auto& Event : Diag.RecentMediaEvents )
		{
			crow::json::wvalue Item;
			Item["sequence"] = Event.Sequence;
			Item["activityId"] = Event.ActivityID;
			Item["packetSequence"] = Event.PacketSequence;
			Item["timestampUnixMs"] = Event.TimestampUnixMs;
			Item["elapsedMs"] = Event.ElapsedMs;
			Item["lastTimestampUnixMs"] = Event.LastTimestampUnixMs ? Event.LastTimestampUnixMs : Event.TimestampUnixMs;
			Item["count"] = Event.Count;
			Item["generation"] = Event.Generation;
			Item["segmentIndex"] = Event.SegmentIndex;
			Item["partialIndex"] = Event.PartialIndex;
			Item["category"] = Event.Category;
			Item["severity"] = Event.Severity;
			Item["phase"] = Event.Phase;
			Item["component"] = Event.Component;
			Item["message"] = Event.Message;
			if( !Event.LastMessage.empty() ) Item["lastMessage"] = Event.LastMessage;
			Item["disposition"] = Event.Disposition;
			Item["audio"] = Event.Audio;
			Item["keyframe"] = Event.Keyframe;
			Item["corrupt"] = Event.Corrupt;
			Item["packetSize"] = Event.PacketSize;
			Item["sourceDtsUs"] = Event.HasSourceDts ?
				crow::json::wvalue( Event.SourceDtsUs ) : crow::json::wvalue( nullptr );
			Item["sourcePtsUs"] = Event.HasSourcePts ?
				crow::json::wvalue( Event.SourcePtsUs ) : crow::json::wvalue( nullptr );
			MediaEvents.push_back( std::move( Item ) );
		}
		Value["recentMediaEvents"] = std::move( MediaEvents );
		return Value;
	};

	std::vector<crow::json::wvalue> Cameras;
	Cameras.reserve( CameraSnapshots.size() );
	for( const auto& Snapshot : CameraSnapshots )
	{
		crow::json::wvalue Camera;
		Camera["cameraId"] = Snapshot.Id;
		Camera["name"] = Snapshot.Name;
		Camera["state"] = Snapshot.Status;

		if( m_GlobalContext->CommonImageProcessingJobQueue )
		{
			auto Stats = m_GlobalContext->CommonImageProcessingJobQueue->GetStats( Snapshot.Id );
			crow::json::wvalue Processing;
			Processing["ingressFrames"] = Stats.IngressFrames;
			Processing["startedFrames"] = Stats.StartedFrames;
			Processing["completedFrames"] = Stats.FrameCount;
			Processing["coalescedFrames"] = Stats.CoalescedFrames;
			Processing["coalescedAIFrames"] = Stats.CoalescedAIFrames;
			Processing["pendingEssential"] = Stats.PendingEssentialJobs;
			Processing["pendingAI"] = Stats.PendingAIJobs;
			Processing["oldestPendingEssentialMs"] =
				(double)Stats.OldestPendingEssentialAgeNS / 1000000.0;
			Processing["oldestPendingAIMs"] = (double)Stats.OldestPendingAIAgeNS / 1000000.0;
			Processing["activeJobMs"] = (double)Stats.ActiveJobAgeNS / 1000000.0;
			Processing["ingressWaitSamples"] = Stats.IngressQueueWaitSamples;
			Processing["ingressWaitMeanMs"] = Stats.IngressQueueWaitSamples ?
				crow::json::wvalue( MeanQueueMS( Stats.IngressQueueWaitTotalNS,
					Stats.IngressQueueWaitSamples ) ) : crow::json::wvalue( nullptr );
			Processing["ingressWaitMaxMs"] = Stats.IngressQueueWaitSamples ?
				crow::json::wvalue( (double)Stats.IngressQueueWaitMaxNS / 1000000.0 ) :
				crow::json::wvalue( nullptr );
			Processing["continuationWaitSamples"] = Stats.ContinuationQueueWaitSamples;
			Processing["continuationWaitMeanMs"] = Stats.ContinuationQueueWaitSamples ?
				crow::json::wvalue( MeanQueueMS( Stats.ContinuationQueueWaitTotalNS,
					Stats.ContinuationQueueWaitSamples ) ) : crow::json::wvalue( nullptr );
			Processing["continuationWaitMaxMs"] = Stats.ContinuationQueueWaitSamples ?
				crow::json::wvalue( (double)Stats.ContinuationQueueWaitMaxNS / 1000000.0 ) :
				crow::json::wvalue( nullptr );
			Processing["aiWaitSamples"] = Stats.AIQueueWaitSamples;
			Processing["aiWaitMeanMs"] = Stats.AIQueueWaitSamples ?
				crow::json::wvalue( MeanQueueMS( Stats.AIQueueWaitTotalNS,
					Stats.AIQueueWaitSamples ) ) : crow::json::wvalue( nullptr );
			Processing["aiWaitMaxMs"] = Stats.AIQueueWaitSamples ?
				crow::json::wvalue( (double)Stats.AIQueueWaitMaxNS / 1000000.0 ) :
				crow::json::wvalue( nullptr );
			Processing["processingJobActive"] = Stats.ProcessingJobActive;
			Processing["aiReservationActive"] = Stats.AIReservationActive;
			Camera["processing"] = std::move( Processing );
		}

		std::vector<crow::json::wvalue> Streams;
		if( Snapshot.Worker )
		{
			Streams.push_back( StreamJson( "main", Snapshot.Status == "Connected",
				Snapshot.Worker->GetLiveStream() ) );
			auto SubWorker = Snapshot.Worker->GetSubStreamWorker();
			if( SubWorker )
				Streams.push_back( StreamJson( "preview", SubWorker->IsConnected(),
					SubWorker->GetLiveStream() ) );
		}
		Camera["streams"] = std::move( Streams );
		Cameras.push_back( std::move( Camera ) );
	}

	const auto NowSteady = std::chrono::steady_clock::now();
	crow::json::wvalue Server;
	Server["instanceId"] = std::format( "{}-{}", GetCurrentProcessId(),
		std::chrono::duration_cast<std::chrono::milliseconds>(
			HealthProcessStart.time_since_epoch() ).count() );
	Server["webBuildHash"] = BuildHash;
	Server["collectionMode"] = "onDemand";
	Server["requestSequence"] = ++HealthSampleSequence;
	Server["uptimeMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
		NowSteady - HealthProcessStart ).count();
	Server["host"] = std::move( Host );
	crow::json::wvalue Audio;
	const auto& AudioHealth = m_GlobalContext->AudioIntelligence;
	const uint64_t AudioClips = AudioHealth.ClipsProcessed.load();
	Audio["workerLoaded"] = AudioHealth.WorkerLoaded.load();
	Audio["clipsProcessed"] = AudioClips;
	Audio["eventsProduced"] = AudioHealth.EventsProduced.load();
	Audio["decodeFailures"] = AudioHealth.DecodeFailures.load();
	Audio["inferenceFailures"] = AudioHealth.InferenceFailures.load();
	Audio["lastClipUID"] = AudioHealth.LastClipUID.load();
	Audio["lastInferenceMs"] = static_cast<double>( AudioHealth.LastInferenceUS.load() ) / 1000.0;
	Audio["meanInferenceMs"] = AudioClips ?
		static_cast<double>( AudioHealth.TotalInferenceUS.load() ) / ( 1000.0 * AudioClips ) : 0.0;
	Server["audioIntelligence"] = std::move( Audio );

	const auto CurrentHttpRequest = m_App.get_context<HttpTracingMiddleware>( req ).Token;
	const auto HttpSnapshot = Witness::HttpDiagnostics::GetSnapshot( CurrentHttpRequest.Id );
	crow::json::wvalue Http;
	Http["started"] = HttpSnapshot.Started;
	Http["completed"] = HttpSnapshot.Completed;
	Http["errors"] = HttpSnapshot.Errors;
	Http["slowRequests"] = HttpSnapshot.Slow;
	Http["inFlight"] = HttpSnapshot.InFlight;
	Http["maxInFlight"] = HttpSnapshot.MaxInFlight;
	Http["observedWorkers"] = HttpSnapshot.ObservedWorkers;
	auto DurationJson = []( const Witness::HttpDiagnostics::DurationSummary& Summary )
	{
		crow::json::wvalue Value;
		Value["p50Ms"] = Summary.P50Ms;
		Value["p95Ms"] = Summary.P95Ms;
		Value["p99Ms"] = Summary.P99Ms;
		Value["maxMs"] = Summary.MaxMs;
		return Value;
	};
	Http["handlerDuration"] = DurationJson( HttpSnapshot.Handler );
	Http["eventLoopOccupancy"] = DurationJson( HttpSnapshot.EventLoop );
	std::vector<crow::json::wvalue> HttpRoutes;
	HttpRoutes.reserve( HttpSnapshot.Routes.size() );
	for( const auto& Route : HttpSnapshot.Routes )
	{
		crow::json::wvalue Value;
		Value["route"] = Route.Route;
		Value["started"] = Route.Started;
		Value["completed"] = Route.Completed;
		Value["errors"] = Route.Errors;
		Value["slowRequests"] = Route.Slow;
		Value["meanHandlerMs"] = Route.MeanHandlerMs;
		Value["maxHandlerMs"] = Route.MaxHandlerMs;
		Value["meanEventLoopMs"] = Route.MeanEventLoopMs;
		Value["maxEventLoopMs"] = Route.MaxEventLoopMs;
		HttpRoutes.push_back( std::move( Value ) );
	}
	Http["routes"] = std::move( HttpRoutes );
	std::vector<crow::json::wvalue> SlowRequests;
	SlowRequests.reserve( HttpSnapshot.RecentSlowRequests.size() );
	for( const auto& Request : HttpSnapshot.RecentSlowRequests )
	{
		crow::json::wvalue Value;
		Value["sequence"] = Request.Sequence;
		Value["startedUnixMs"] = Request.StartedUnixMs;
		Value["method"] = Request.Method;
		Value["route"] = Request.Route;
		Value["path"] = Request.Path;
		Value["worker"] = Request.Worker;
		Value["thread"] = Request.Thread;
		Value["status"] = Request.Status;
		Value["requestBytes"] = Request.RequestBytes;
		Value["responseBytes"] = Request.ResponseBytes;
		Value["handlerMs"] = Request.HandlerMs;
		Value["eventLoopMs"] = Request.EventLoopMs;
		Value["postHandlerDelayMs"] = Request.PostHandlerDelayMs;
		Value["asyncCompletion"] = Request.AsyncCompletion;
		SlowRequests.push_back( std::move( Value ) );
	}
	Http["recentSlowRequests"] = std::move( SlowRequests );
	std::vector<crow::json::wvalue> ActiveRequests;
	ActiveRequests.reserve( HttpSnapshot.ActiveRequests.size() );
	for( const auto& Request : HttpSnapshot.ActiveRequests )
	{
		crow::json::wvalue Value;
		Value["sequence"] = Request.Sequence;
		Value["startedUnixMs"] = Request.StartedUnixMs;
		Value["method"] = Request.Method;
		Value["route"] = Request.Route;
		Value["path"] = Request.Path;
		Value["worker"] = Request.Worker;
		Value["thread"] = Request.Thread;
		Value["elapsedMs"] = Request.ElapsedMs;
		Value["handlerComplete"] = Request.HandlerComplete;
		Value["waitingForEventLoop"] = Request.WaitingForEventLoop;
		ActiveRequests.push_back( std::move( Value ) );
	}
	Http["activeRequests"] = std::move( ActiveRequests );
	Server["http"] = std::move( Http );

	const auto SQLiteSnapshot = GetSQLiteDiagnosticsSnapshot();
	crow::json::wvalue SQLite;
	SQLite["queries"] = SQLiteSnapshot.Queries;
	SQLite["contendedQueries"] = SQLiteSnapshot.ContendedQueries;
	SQLite["slowQueries"] = SQLiteSnapshot.SlowQueries;
	SQLite["meanMutexWaitMs"] = SQLiteSnapshot.MeanMutexWaitMs;
	SQLite["maxMutexWaitMs"] = SQLiteSnapshot.MaxMutexWaitMs;
	SQLite["meanScopeMs"] = SQLiteSnapshot.MeanScopeMs;
	SQLite["maxScopeMs"] = SQLiteSnapshot.MaxScopeMs;
	SQLite["meanExecuteMs"] = SQLiteSnapshot.MeanExecuteMs;
	SQLite["maxExecuteMs"] = SQLiteSnapshot.MaxExecuteMs;
	std::vector<crow::json::wvalue> SlowQueries;
	SlowQueries.reserve( SQLiteSnapshot.RecentSlowQueries.size() );
	for( const auto& Query : SQLiteSnapshot.RecentSlowQueries )
	{
		crow::json::wvalue Value;
		Value["sequence"] = Query.Sequence;
		Value["startedUnixMs"] = Query.StartedUnixMs;
		Value["query"] = Query.Query;
		Value["thread"] = Query.Thread;
		Value["mutexWaitMs"] = Query.MutexWaitMs;
		Value["scopeMs"] = Query.ScopeMs;
		Value["executeMs"] = Query.ExecuteMs;
		SlowQueries.push_back( std::move( Value ) );
	}
	SQLite["recentSlowQueries"] = std::move( SlowQueries );
	Server["database"] = std::move( SQLite );

	crow::json::wvalue Coverage;
	Coverage["hostCpuPercent"] = "notImplemented";
	Coverage["rtpSequenceCounters"] = "notExposedByDemuxer";
	Coverage["hardwareDecoderSessions"] = "notExposedByBrowser";
	Coverage["otherBrowserClients"] = "notCollectedInV1";
	Coverage["rateCalculationSupported"] = false;
	Coverage["rateCalculationReason"] = "worker and processing counter epochs are not yet exposed";
	Coverage["mediaEventFeed"] = "last 48 warning, error, packet disposition, and recovery events per stream";
	Coverage["httpTracing"] =
		"handler time ends before Crow's synchronous response write; event-loop release delay includes that write and queued callbacks; percentiles use the latest 1024 finalized requests";
	Coverage["databaseTracing"] =
		"prepared-query mutex wait >=25ms or query scope >=100ms retained in the last 64 events; execute timing measures sqlite3_step calls only";

	const auto CollectionEnd = std::chrono::steady_clock::now();
	crow::json::wvalue Data;
	Data["schemaVersion"] = 4;
	Data["sampledAtUtc"] = std::format( "{:%Y-%m-%dT%H:%M:%S}Z",
		std::chrono::system_clock::now() );
	Data["collectionStartedMonotonicMs"] =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			CollectionStart - HealthProcessStart ).count();
	Data["collectionEndedMonotonicMs"] =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			CollectionEnd - HealthProcessStart ).count();
	Data["collectionDurationMs"] =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			CollectionEnd - CollectionStart ).count();
	Data["server"] = std::move( Server );
	Data["cameras"] = std::move( Cameras );
	Data["coverage"] = std::move( Coverage );

	res.set_header( "Content-Type", "application/json" );
	res.set_header( "Cache-Control", "no-store" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugStreamingDiag( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	struct CameraDiagnosticSnapshot
	{
		int Id = 0;
		std::string Name;
		std::string Status;
		bool LowLatencyHLS = false;
		std::shared_ptr<Witness::Camera::LiveOutputStream> LiveStream;
	};

	// Camera workers need the global context for status updates and lifecycle
	// events. Only copy stable handles while holding it: constructing the packet
	// trace JSON can take seconds for a busy multi-camera server.
	std::vector<CameraDiagnosticSnapshot> CameraSnapshots;
	{
		std::shared_lock<std::shared_mutex> lock( m_GlobalContext->Mutex );
		for( auto& [id, state] : m_GlobalContext->GetCameraMap() )
		{
			CameraDiagnosticSnapshot Snapshot;
			Snapshot.Id = id;
			Snapshot.Name = state.Name;
			Snapshot.Status = state.Status;
			if( state.Worker )
			{
				Snapshot.LowLatencyHLS = state.Worker->GetCameraSettings().LowLatencyHLS != 0;
				Snapshot.LiveStream = state.Worker->GetLiveStream();
			}
			CameraSnapshots.push_back( std::move( Snapshot ) );
		}
	}

	std::vector<crow::json::wvalue> CameraArray;
	CameraArray.reserve( CameraSnapshots.size() );
	for( auto& Snapshot : CameraSnapshots )
	{
		crow::json::wvalue CamData;
		CamData["id"] = Snapshot.Id;
		CamData["name"] = Snapshot.Name;
		CamData["status"] = Snapshot.Status;
		CamData["lowLatencyHLS"] = Snapshot.LowLatencyHLS;

		if( m_GlobalContext->CommonImageProcessingJobQueue )
		{
			auto QueueStats = m_GlobalContext->CommonImageProcessingJobQueue->GetStats( Snapshot.Id );
			auto MeanQueueMS = []( int64_t TotalNS, uint64_t Samples )
			{
				return Samples ? (double)TotalNS / ((double)Samples * 1000.0 * 1000.0) : 0.0;
			};
			crow::json::wvalue QueueData;
			QueueData["ingressFrames"] = QueueStats.IngressFrames;
			QueueData["startedFrames"] = QueueStats.StartedFrames;
			QueueData["coalescedFrames"] = QueueStats.CoalescedFrames;
			QueueData["coalescedAIFrames"] = QueueStats.CoalescedAIFrames;
			QueueData["pendingEssential"] = QueueStats.PendingEssentialJobs;
			QueueData["pendingAI"] = QueueStats.PendingAIJobs;
			QueueData["peakPendingEssential"] = QueueStats.PeakPendingEssentialJobs;
			QueueData["peakPendingAI"] = QueueStats.PeakPendingAIJobs;
			QueueData["ingressWaitMeanMs"] = MeanQueueMS(
				QueueStats.IngressQueueWaitTotalNS, QueueStats.IngressQueueWaitSamples );
			QueueData["ingressWaitMaxMs"] = (double)QueueStats.IngressQueueWaitMaxNS / 1000000.0;
			QueueData["continuationWaitMeanMs"] = MeanQueueMS(
				QueueStats.ContinuationQueueWaitTotalNS, QueueStats.ContinuationQueueWaitSamples );
			QueueData["continuationWaitMaxMs"] = (double)QueueStats.ContinuationQueueWaitMaxNS / 1000000.0;
			QueueData["aiWaitMeanMs"] = MeanQueueMS(
				QueueStats.AIQueueWaitTotalNS, QueueStats.AIQueueWaitSamples );
			QueueData["aiWaitMaxMs"] = (double)QueueStats.AIQueueWaitMaxNS / 1000000.0;
			QueueData["oldestPendingEssentialMs"] = (double)QueueStats.OldestPendingEssentialAgeNS / 1000000.0;
			QueueData["oldestPendingAIMs"] = (double)QueueStats.OldestPendingAIAgeNS / 1000000.0;
			QueueData["activeJobMs"] = (double)QueueStats.ActiveJobAgeNS / 1000000.0;
			QueueData["processingJobActive"] = QueueStats.ProcessingJobActive;
			QueueData["aiReservationActive"] = QueueStats.AIReservationActive;
			QueueData["activeProcessingSources"] = QueueStats.ActiveProcessingSources;
			QueueData["activeAISources"] = QueueStats.ActiveAISources;
			QueueData["activeBackgroundAIJobs"] = QueueStats.ActiveBackgroundAIJobs;
			QueueData["maximumConcurrentAIJobs"] = QueueStats.MaximumConcurrentAIJobs;
			CamData["processingQueue"] = std::move( QueueData );
		}

		if( Snapshot.LiveStream )
		{
				auto Diag = Snapshot.LiveStream->GetStreamingDiagnostics();

					crow::json::wvalue StreamData;
					auto StructureErrorName = []( int Error )
					{
						switch( Error )
						{
						case 0: return "none";
						case 1: return "truncatedHeader";
						case 2: return "invalidBoxSize";
						case 3: return "boxBeyondFragment";
						case 4: return "missingInitBoxes";
						case 5: return "incompleteMediaPair";
						default: return "unknown";
						}
					};
					StreamData["diagnosticsSchemaVersion"] = 7;
					StreamData["packetTraceCapacity"] = 1024;
					StreamData["totalSegments"] = Diag.TotalSegments;
					StreamData["reconnectCount"] = Diag.ReconnectCount;
					StreamData["totalDtsDuration"] = Diag.TotalDtsDuration;
					StreamData["totalAccumulatedDuration"] = Diag.TotalAccumulatedDuration;
					StreamData["cumulativeDriftMs"] = (Diag.TotalAccumulatedDuration - Diag.TotalDtsDuration) * 1000.0;
					StreamData["maxSingleSegmentDriftMs"] = Diag.MaxDriftMs;
					StreamData["currentSegmentIndex"] = Diag.CurrentSegmentIndex;
					StreamData["backlogSize"] = Diag.BacklogSize;
					StreamData["initGeneration"] = Diag.InitGeneration;
					StreamData["timestampNormalizationActive"] = Diag.TimestampNormalizationActive;
					StreamData["acceptedVideoPackets"] = (int64_t)Diag.AcceptedVideoPackets;
					StreamData["acceptedVideoKeyframes"] = (int64_t)Diag.AcceptedVideoKeyframes;
					StreamData["repairedVideoTimestamps"] = (int64_t)Diag.RepairedVideoTimestamps;
					StreamData["droppedVideoPackets"] = (int64_t)Diag.DroppedVideoPackets;
					StreamData["missingVideoDtsPackets"] = (int64_t)Diag.MissingVideoDtsPackets;
					StreamData["corruptVideoPackets"] = (int64_t)Diag.CorruptVideoPackets;
					StreamData["streamEstablished"] = Diag.StreamEstablished;
					StreamData["startupGraceElapsedMs"] = Diag.StartupGraceElapsedMs;
					StreamData["startupAcceptedVideoPackets"] = (int64_t)Diag.StartupAcceptedVideoPackets;
					StreamData["startupDroppedVideoPackets"] = (int64_t)Diag.StartupDroppedVideoPackets;
					StreamData["startupRepairedVideoTimestamps"] = (int64_t)Diag.StartupRepairedVideoTimestamps;
					StreamData["establishedAcceptedVideoPackets"] = (int64_t)Diag.EstablishedAcceptedVideoPackets;
					StreamData["establishedDroppedVideoPackets"] = (int64_t)Diag.EstablishedDroppedVideoPackets;
					StreamData["establishedRepairedVideoTimestamps"] = (int64_t)Diag.EstablishedRepairedVideoTimestamps;
					crow::json::wvalue DropReasons;
					DropReasons["waitingForKeyframe"] = Diag.WaitingForKeyframePackets;
					DropReasons["missingTimestamp"] = Diag.MissingTimestampPackets;
					DropReasons["beforeVideoEpoch"] = Diag.BeforeVideoEpochPackets;
					DropReasons["negativeTimestamp"] = Diag.NegativeTimestampPackets;
					DropReasons["nonMonotonicInput"] = Diag.NonMonotonicInputPackets;
					DropReasons["noMuxBuffer"] = Diag.NoMuxBufferPackets;
					DropReasons["nonMonotonicOutput"] = Diag.NonMonotonicOutputPackets;
					DropReasons["muxError"] = Diag.MuxErrorPackets;
					DropReasons["decodeCorruption"] = Diag.DecodeCorruptionEvents;
					DropReasons["decodeRecovery"] = Diag.DecodeRecoveryEvents;
					StreamData["packetDispositionCounts"] = std::move( DropReasons );
					StreamData["videoPhaseErrorMs"] = Diag.VideoPhaseErrorMs;
					StreamData["videoCorrectionMs"] = Diag.VideoCorrectionMs;
					StreamData["audioVideoSkewMs"] = Diag.AudioVideoSkewMs;
					StreamData["timestampCorrectionSaturatedPackets"] =
						(int64_t)Diag.TimestampCorrectionSaturatedPackets;
					StreamData["videoCodec"] = Diag.VideoCodec;
					StreamData["audioCodec"] = Diag.AudioCodec;
					StreamData["inputFormat"] = Diag.InputFormat;
					crow::json::wvalue VideoFormat;
					VideoFormat["profile"] = Diag.VideoProfile;
					VideoFormat["level"] = Diag.VideoLevel;
					VideoFormat["width"] = Diag.VideoWidth;
					VideoFormat["height"] = Diag.VideoHeight;
					VideoFormat["timeBaseNum"] = Diag.VideoTimeBaseNum;
					VideoFormat["timeBaseDen"] = Diag.VideoTimeBaseDen;
					VideoFormat["extradataBytes"] = Diag.VideoExtradataBytes;
					VideoFormat["extradataHash"] = std::format("{:016x}", Diag.VideoExtradataHash);
					StreamData["videoFormat"] = std::move( VideoFormat );
					if( !Diag.AudioCodec.empty() )
					{
						crow::json::wvalue AudioFormat;
						AudioFormat["profile"] = Diag.AudioProfile;
						AudioFormat["sampleRate"] = Diag.AudioSampleRate;
						AudioFormat["channels"] = Diag.AudioChannels;
						AudioFormat["timeBaseNum"] = Diag.AudioTimeBaseNum;
						AudioFormat["timeBaseDen"] = Diag.AudioTimeBaseDen;
						AudioFormat["extradataBytes"] = Diag.AudioExtradataBytes;
						AudioFormat["extradataHash"] = std::format("{:016x}", Diag.AudioExtradataHash);
						StreamData["audioFormat"] = std::move( AudioFormat );
					}
					crow::json::wvalue InitStructure;
					InitStructure["valid"] = Diag.InitStructureValid;
					InitStructure["boxCount"] = Diag.InitBoxCount;
					InitStructure["ftypCount"] = Diag.InitFtypCount;
					InitStructure["moovCount"] = Diag.InitMoovCount;
					InitStructure["error"] = Diag.InitStructureError;
					InitStructure["errorName"] = StructureErrorName( Diag.InitStructureError );
					InitStructure["errorOffset"] = Diag.InitErrorOffset;
					StreamData["initStructure"] = std::move( InitStructure );

					if( Diag.TotalSegments > 0 )
					{
						StreamData["avgDtsDuration"] = Diag.TotalDtsDuration / Diag.TotalSegments;
						StreamData["avgAccumulatedDuration"] = Diag.TotalAccumulatedDuration / Diag.TotalSegments;
					}

					std::vector<crow::json::wvalue> Segments;
					for( auto& Seg : Diag.RecentSegments )
					{
						crow::json::wvalue S;
						S["idx"] = Seg.SegmentIndex;
						S["dtsDur"] = Seg.DtsDuration;
						S["outputDur"] = Seg.OutputDuration;
						S["accDur"] = Seg.AccumulatedDuration;
						S["driftMs"] = Seg.DriftMs;
						S["timestampNormalized"] = Seg.TimestampNormalizationActive;
						S["acceptedVideoPackets"] = (int64_t)Seg.AcceptedVideoPackets;
						S["acceptedVideoKeyframes"] = (int64_t)Seg.AcceptedVideoKeyframes;
						S["repairedVideoTimestamps"] = (int64_t)Seg.RepairedVideoTimestamps;
						S["droppedVideoPackets"] = (int64_t)Seg.DroppedVideoPackets;
						S["missingVideoDtsPackets"] = (int64_t)Seg.MissingVideoDtsPackets;
						S["corruptVideoPackets"] = (int64_t)Seg.CorruptVideoPackets;
						S["packetPayloadHash"] = std::format("{:016x}", Seg.PacketPayloadHash);
						S["fragmentHash"] = std::format("{:016x}", Seg.FragmentHash);
						S["fragmentBytes"] = (int64_t)Seg.FragmentBytes;
						S["fragmentStructureValid"] = Seg.FragmentStructureValid;
						S["fragmentBoxCount"] = Seg.FragmentBoxCount;
						S["fragmentMoofCount"] = Seg.FragmentMoofCount;
						S["fragmentMdatCount"] = Seg.FragmentMdatCount;
						S["fragmentStructureError"] = Seg.FragmentStructureError;
						S["fragmentStructureErrorName"] = StructureErrorName( Seg.FragmentStructureError );
						S["fragmentErrorOffset"] = Seg.FragmentErrorOffset;
						Segments.push_back( std::move( S ) );
					}
					StreamData["recentSegments"] = std::move( Segments );

					std::vector<crow::json::wvalue> Fragments;
					Fragments.reserve( Diag.RecentFragments.size() );
					for( const auto& Fragment : Diag.RecentFragments )
					{
						crow::json::wvalue F;
						F["generation"] = Fragment.Generation;
						F["segment"] = Fragment.SegmentIndex;
						F["part"] = Fragment.PartIndex;
						F["independent"] = Fragment.Independent;
						F["keyframeSeekSafe"] = Fragment.KeyframeSeekSafe;
						F["bytes"] = Fragment.Bytes;
						F["hash"] = std::format("{:016x}", Fragment.Hash);
						F["structureValid"] = Fragment.StructureValid;
						F["boxCount"] = Fragment.BoxCount;
						F["moofCount"] = Fragment.MoofCount;
						F["mdatCount"] = Fragment.MdatCount;
						F["structureError"] = Fragment.StructureError;
						F["structureErrorName"] = StructureErrorName( Fragment.StructureError );
						F["errorOffset"] = Fragment.ErrorOffset;
						Fragments.push_back( std::move( F ) );
					}
					StreamData["recentFragments"] = std::move( Fragments );

					std::vector<crow::json::wvalue> Packets;
					Packets.reserve( Diag.RecentPackets.size() );
					auto PrefixHex = []( const uint8_t* Bytes, int Length )
					{
						static constexpr char Hex[] = "0123456789abcdef";
						std::string Value;
						Value.resize( Length * 2 );
						for( int Index = 0; Index < Length; ++Index )
						{
							Value[Index * 2] = Hex[Bytes[Index] >> 4];
							Value[Index * 2 + 1] = Hex[Bytes[Index] & 0x0f];
						}
						return Value;
					};
					auto CodecUnitName = [&Diag]( int Type ) -> const char*
					{
						if( Diag.VideoCodec == "h264" )
						{
							switch( Type )
							{
							case 1: return "nonIdrSlice";
							case 5: return "idrSlice";
							case 6: return "sei";
							case 7: return "sps";
							case 8: return "pps";
							case 9: return "aud";
							default: return Type >= 1 && Type <= 5 ? "vcl" : "other";
							}
						}
						if( Diag.VideoCodec == "hevc" )
						{
							switch( Type )
							{
							case 19: return "idrWithRadl";
							case 20: return "idrNoLeadingPictures";
							case 21: return "cra";
							case 32: return "vps";
							case 33: return "sps";
							case 34: return "pps";
							case 35: return "aud";
							case 39: return "prefixSei";
							case 40: return "suffixSei";
							default: return Type >= 0 && Type <= 31 ? "vcl" : "other";
							}
						}
						return "unknown";
					};
					auto SerializePacket = [&PrefixHex, &CodecUnitName]( const auto& Packet )
					{
						crow::json::wvalue P;
						P["seq"] = (int64_t)Packet.Sequence;
						P["generation"] = Packet.Generation;
						P["segment"] = Packet.SegmentIndex;
						P["part"] = Packet.PartialIndex;
						P["track"] = Packet.Audio ? "audio" : "video";
						P["disposition"] = Packet.Disposition;
						P["arrivalMs"] = Packet.ArrivalMs;
						P["size"] = Packet.Size;
						P["flags"] = Packet.Flags;
						P["payloadHash"] = std::format("{:016x}", Packet.PayloadHash);
						P["payloadPrefix"] = PrefixHex( Packet.PayloadPrefix, Packet.PayloadPrefixLength );
						const char* Packetization = "opaque";
						if( Packet.Packetization == 1 ) Packetization = "annexB";
						else if( Packet.Packetization == 2 ) Packetization = "lengthPrefixed";
						else if( Packet.Packetization == 3 ) Packetization = "adts";
						P["packetization"] = Packetization;
						P["codecUnitCount"] = Packet.CodecUnitCount;
						if( Packet.PrimaryCodecUnitType >= 0 )
						{
							P["primaryCodecUnitType"] = Packet.PrimaryCodecUnitType;
							P["primaryCodecUnitName"] = CodecUnitName( Packet.PrimaryCodecUnitType );
						}
						P["keyframe"] = Packet.Keyframe;
						P["corrupt"] = Packet.Corrupt;
						P["dtsSynthesized"] = Packet.DtsSynthesized;
						P["ptsSynthesized"] = Packet.PtsSynthesized;
						P["durationSynthesized"] = Packet.DurationSynthesized;
						P["timestampNormalized"] = Packet.TimestampNormalized;
						P["timestampRepaired"] = Packet.TimestampRepaired;
						P["correctionSaturated"] = Packet.CorrectionSaturated;
						if( Packet.HasSourceDts ) P["sourceDtsUs"] = Packet.SourceDtsUs;
						if( Packet.HasSourcePts ) P["sourcePtsUs"] = Packet.SourcePtsUs;
						P["sourceDurationUs"] = Packet.SourceDurationUs;
						if( Packet.HasOutputDts ) P["outputDtsUs"] = Packet.OutputDtsUs;
						if( Packet.HasOutputPts ) P["outputPtsUs"] = Packet.OutputPtsUs;
						P["outputDurationUs"] = Packet.OutputDurationUs;
						return P;
					};
					for( const auto& Packet : Diag.RecentPackets )
					{
						Packets.push_back( SerializePacket( Packet ) );
					}
					StreamData["recentPackets"] = std::move( Packets );

					std::vector<crow::json::wvalue> MediaEvents;
					MediaEvents.reserve( Diag.RecentMediaEvents.size() );
					for( const auto& Event : Diag.RecentMediaEvents )
					{
						crow::json::wvalue Item;
						Item["sequence"] = Event.Sequence;
						Item["activityId"] = Event.ActivityID;
						Item["packetSequence"] = Event.PacketSequence;
						Item["timestampUnixMs"] = Event.TimestampUnixMs;
						Item["elapsedMs"] = Event.ElapsedMs;
						Item["lastTimestampUnixMs"] = Event.LastTimestampUnixMs ? Event.LastTimestampUnixMs : Event.TimestampUnixMs;
						Item["count"] = Event.Count;
						Item["generation"] = Event.Generation;
						Item["segmentIndex"] = Event.SegmentIndex;
						Item["partialIndex"] = Event.PartialIndex;
						Item["category"] = Event.Category;
						Item["severity"] = Event.Severity;
						Item["phase"] = Event.Phase;
						Item["component"] = Event.Component;
						Item["message"] = Event.Message;
						if( !Event.LastMessage.empty() ) Item["lastMessage"] = Event.LastMessage;
						Item["disposition"] = Event.Disposition;
						Item["audio"] = Event.Audio;
						Item["keyframe"] = Event.Keyframe;
						Item["corrupt"] = Event.Corrupt;
						Item["packetSize"] = Event.PacketSize;
						if( Event.HasSourceDts ) Item["sourceDtsUs"] = Event.SourceDtsUs;
						if( Event.HasSourcePts ) Item["sourcePtsUs"] = Event.SourcePtsUs;
						MediaEvents.push_back( std::move( Item ) );
					}
					StreamData["recentMediaEvents"] = std::move( MediaEvents );

					std::vector<crow::json::wvalue> Anomalies;
					Anomalies.reserve( Diag.Anomalies.size() );
					for( const auto& Anomaly : Diag.Anomalies )
					{
						crow::json::wvalue A;
						A["sequence"] = Anomaly.Sequence;
						A["capturedAtMs"] = Anomaly.CapturedAtMs;
						A["generation"] = Anomaly.Generation;
						A["segment"] = Anomaly.SegmentIndex;
						A["reason"] = Anomaly.Reason;
						std::vector<crow::json::wvalue> AnomalyFragments;
						AnomalyFragments.reserve( Anomaly.Fragments.size() );
						for( const auto& Fragment : Anomaly.Fragments )
						{
							crow::json::wvalue F;
							F["generation"] = Fragment.Generation;
							F["segment"] = Fragment.SegmentIndex;
							F["part"] = Fragment.PartIndex;
							F["independent"] = Fragment.Independent;
							F["keyframeSeekSafe"] = Fragment.KeyframeSeekSafe;
							F["bytes"] = Fragment.Bytes;
							F["hash"] = std::format("{:016x}", Fragment.Hash);
							F["structureValid"] = Fragment.StructureValid;
							F["boxCount"] = Fragment.BoxCount;
							F["moofCount"] = Fragment.MoofCount;
							F["mdatCount"] = Fragment.MdatCount;
							F["structureError"] = Fragment.StructureError;
							F["structureErrorName"] = StructureErrorName( Fragment.StructureError );
							F["errorOffset"] = Fragment.ErrorOffset;
							AnomalyFragments.push_back( std::move( F ) );
						}
						A["fragments"] = std::move( AnomalyFragments );
						std::vector<crow::json::wvalue> AnomalyPackets;
						AnomalyPackets.reserve( Anomaly.Packets.size() );
						for( const auto& Packet : Anomaly.Packets )
							AnomalyPackets.push_back( SerializePacket( Packet ) );
						A["packets"] = std::move( AnomalyPackets );
						Anomalies.push_back( std::move( A ) );
					}
					StreamData["anomalies"] = std::move( Anomalies );

			CamData["streaming"] = std::move( StreamData );
		}

		CameraArray.push_back( std::move( CamData ) );
	}

	crow::json::wvalue Data;
	Data["timestamp"] = std::format( "{:%Y-%m-%dT%H:%M:%S}", std::chrono::system_clock::now() );
	Data["cameras"] = std::move( CameraArray );

	// Read today's log file and filter for [HLS] and [DVR] lines
	std::string logDir = ::Witness::LogGetDirectory();
	if( !logDir.empty() )
	{
		auto now = std::chrono::system_clock::now();
		auto time_t = std::chrono::system_clock::to_time_t( now );
		struct tm tm_buf;
#ifdef _WIN32
		localtime_s( &tm_buf, &time_t );
#else
		localtime_r( &time_t, &tm_buf );
#endif
		char dateBuf[16];
		snprintf( dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
			tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday );

		std::string logPath = logDir + "/witness-" + dateBuf + ".log";
		std::ifstream logFile( logPath );
		if( logFile.is_open() )
		{
			std::vector<crow::json::wvalue> LogLines;
			std::string line;
			while( std::getline( logFile, line ) )
			{
				if( line.find( "[HLS]" ) != std::string::npos ||
					line.find( "[DVR]" ) != std::string::npos )
				{
					LogLines.push_back( line );
				}
			}
			Data["serverLog"] = std::move( LogLines );
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugDisk( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	crow::json::wvalue Data;

	try
	{
	// Disk space info
	std::error_code ec;
	auto spaceInfo = std::filesystem::space( m_GlobalContext->CachePath, ec );
	if( !ec )
	{
		Data["diskTotal"] = static_cast<int64_t>(spaceInfo.capacity);
		Data["diskFree"] = static_cast<int64_t>(spaceInfo.available);
		Data["diskUsed"] = static_cast<int64_t>(spaceInfo.capacity - spaceInfo.available);
		Data["diskTotalGB"] = static_cast<double>(spaceInfo.capacity) / (1024.0 * 1024 * 1024);
		Data["diskFreeGB"] = static_cast<double>(spaceInfo.available) / (1024.0 * 1024 * 1024);
	}

	// Continuous segment totals
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousTotalSize" );
		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			Data["segmentCount"] = q.GetColumnValueInt64(0);
			Data["segmentTotalDuration"] = q.GetColumnValueInt64(1);
			Data["segmentTotalBytes"] = q.GetColumnValueInt64(2);
			Data["segmentTotalGB"] = static_cast<double>(q.GetColumnValueInt64(2)) / (1024.0 * 1024 * 1024);
			return true;
		});
	}

	// Per-camera breakdown
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSizePerCamera" );
		std::vector<crow::json::wvalue> cameras;
		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			crow::json::wvalue cam;
			cam["cameraId"] = q.GetColumnValueInt64(0);
			cam["segmentCount"] = q.GetColumnValueInt64(1);
			cam["totalBytes"] = q.GetColumnValueInt64(2);
			cam["totalGB"] = static_cast<double>(q.GetColumnValueInt64(2)) / (1024.0 * 1024 * 1024);
			cameras.push_back( std::move(cam) );
			return true;
		});
		Data["cameras"] = std::move(cameras);
	}

	Data["cachePath"] = m_GlobalContext->CachePath;

	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "HandleDebugDisk exception: %s", e.what() );
		Data["error"] = std::string( e.what() );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugDiskScan( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	crow::json::wvalue Data;

	try
	{
		int64_t totalBytes = 0;
		int fileCount = 0;
		std::error_code ec;

		for( auto& entry : std::filesystem::recursive_directory_iterator( m_GlobalContext->CachePath, ec ) )
		{
			if( entry.is_regular_file( ec ) )
			{
				totalBytes += static_cast<int64_t>( entry.file_size( ec ) );
				fileCount++;
			}
		}

		Data["totalBytes"] = totalBytes;
		Data["totalGB"] = static_cast<double>(totalBytes) / (1024.0 * 1024 * 1024);
		Data["fileCount"] = fileCount;
	}
	catch( const std::exception& e )
	{
		Data["error"] = std::string( e.what() );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDetectionQuery( const crow::request& req, crow::response& res, int cameraId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Parse from/to query params (epoch seconds)
	double from = 0, to = 0;
	auto fromParam = req.url_params.get( "from" );
	auto toParam = req.url_params.get( "to" );

	if( !fromParam || !toParam )
	{
		res.code = 400;
		res.body = R"({"error":"Missing from/to query params"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	from = std::stod( fromParam );
	to = std::stod( toParam );

	// Query detection frames with boxes
	crow::json::wvalue result;
	std::vector<crow::json::wvalue> frames;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectDetectionFramesWithBoxes" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@TimestampFrom", from );
		query->Bind( "@TimestampTo", to );

		int64_t currentFrameUID = -1;
		crow::json::wvalue currentFrame;
		std::vector<crow::json::wvalue> currentBoxes;

		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			int64_t frameUID = q.GetColumnValueInt64( 0 );

			if( frameUID != currentFrameUID )
			{
				// Save previous frame
				if( currentFrameUID >= 0 )
				{
					currentFrame["boxes"] = std::move( currentBoxes );
					frames.push_back( std::move( currentFrame ) );
					currentBoxes.clear();

					// Limit response size
					if( frames.size() >= 2000 )
						return true;
				}

				currentFrameUID = frameUID;
				currentFrame = crow::json::wvalue();
				currentFrame["t"] = q.GetColumnValueDouble( 1 );  // timestamp
				currentFrame["w"] = q.GetColumnValueInt( 2 );     // frameWidth
				currentFrame["h"] = q.GetColumnValueInt( 3 );     // frameHeight
			}

			// Add box (LEFT JOIN may produce NULL box columns if frame has no boxes)
			const char* className = q.GetColumnValueText( 6 );
			if( className )
			{
				crow::json::wvalue box;
				box["id"] = q.GetColumnValueInt( 4 );    // TrackingID
				box["cls"] = std::string( className );
				box["conf"] = q.GetColumnValueDouble( 7 );
				box["x"] = q.GetColumnValueDouble( 8 );
				box["y"] = q.GetColumnValueDouble( 9 );
				box["w"] = q.GetColumnValueDouble( 10 );
				box["h"] = q.GetColumnValueDouble( 11 );
				box["baseline"] = q.GetColumnValueInt( 12 ) != 0;
				const char* cropPath = q.GetColumnValueText( 13 );
				if( cropPath )
					box["crop"] = std::string( cropPath );
				const char* faceName = q.GetColumnValueText( 14 );
				if( faceName )
					box["name"] = std::string( faceName );
				currentBoxes.push_back( std::move( box ) );
			}

			return true;
		});

		// Don't forget the last frame
		if( currentFrameUID >= 0 && frames.size() < 2000 )
		{
			currentFrame["boxes"] = std::move( currentBoxes );
			frames.push_back( std::move( currentFrame ) );
		}
	}
	catch( const std::exception& e )
	{
		res.code = 500;
		crow::json::wvalue err;
		err["error"] = std::string( e.what() );
		res.body = err.dump();
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	result["frames"] = std::move( frames );
	result["cameraId"] = cameraId;

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleTrailsQuery( const crow::request& req, crow::response& res, int cameraId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	if( !m_GlobalContext || !m_GlobalContext->Database )
	{
		res.code = 500;
		res.body = R"({"error":"Database not available"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	auto fromParam = req.url_params.get( "from" );
	auto toParam = req.url_params.get( "to" );

	if( !fromParam || !toParam )
	{
		res.code = 400;
		res.body = R"({"error":"Missing from/to query params"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	double from = std::stod( fromParam );
	double to = std::stod( toParam );

	// Build JSON response manually for performance -- avoids crow::json::wvalue overhead
	// and passes PointData through as raw compact arrays
	std::ostringstream json;
	json << R"({"cameraId":)" << cameraId << R"(,"trails":[)";
	bool first = true;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectTrails" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@TimestampFrom", from );
		query->Bind( "@TimestampTo", to );

		query->Execute( [&]( const SQLiteDatabaseQuery& q ) -> bool
		{
			const char* pointData = q.GetColumnValueText( 7 );
			if( !pointData )
				return true;

			if( !first ) json << ",";
			first = false;

			int64_t trailUID = q.GetColumnValueInt64( 0 );
			int64_t clipUID = q.GetColumnValueInt64( 1 );
			const char* className = q.GetColumnValueText( 3 );
			const char* faceName = q.GetColumnValueText( 4 );
			double clipTimestamp = q.GetColumnValueDouble( 8 );
			double clipDuration = q.GetColumnValueDouble( 9 );

			json << R"({"id":)" << trailUID
				 << R"(,"clipId":)" << clipUID
				 << R"(,"clipTs":)" << std::fixed << std::setprecision( 2 ) << clipTimestamp
				 << R"(,"clipDur":)" << clipDuration
				 << R"(,"cls":")" << ( className ? className : "" ) << "\"";

			if( faceName )
				json << R"(,"name":")" << faceName << "\"";

			// Pass PointData through verbatim -- already compact [[x,y,t],...]
			json << R"(,"pts":)" << pointData << "}";

			return true;
		});
	}
	catch( const std::exception& e )
	{
		res.code = 500;
		res.body = std::string( R"({"error":")" ) + e.what() + "\"}";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	json << "]}";

	res.set_header( "Content-Type", "application/json" );
	res.body = json.str();
	res.code = 200;
	res.end();
}

void CrowListener::HandleReprocessQueue( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	crow::json::wvalue result;

	// Get total count
	int totalCount = 0;
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "CountClipsToReprocess" );
		q->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		q->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			totalCount = query.GetColumnValueInt( 0 );
			return true;
		});
	}

	// Get top 100 queued clips
	std::vector<crow::json::wvalue> clips;
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectReprocessQueue" );
		q->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		q->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			crow::json::wvalue clip;
			clip["uid"] = query.GetColumnValueInt64( 0 );
			clip["timestamp"] = query.GetColumnValueInt64( 1 );
			clip["camera"] = query.GetColumnValueInt( 2 );
			clip["detectionVersion"] = query.GetColumnValueInt( 3 );
			clip["recordMode"] = query.GetColumnValueInt( 4 );
			const char* tags = query.GetColumnValueText( 5 );
			clip["tags"] = tags ? std::string( tags ) : "";
			clip["duration"] = query.GetColumnValueDouble( 6 );
			clips.push_back( std::move( clip ) );
			return true;
		});
	}

	result["total"] = totalCount;
	result["clips"] = std::move( clips );
	result["detectionVersion"] = CURRENT_DETECTION_VERSION;

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

#ifdef CROW_ENABLE_SSL
static bool LogCertExpiry( const std::string& certPath )
{
	FILE* fp = nullptr;
#ifdef _WIN32
	fopen_s( &fp, certPath.c_str(), "r" );
#else
	fp = fopen( certPath.c_str(), "r" );
#endif
	if( !fp )
	{
		LOG_ERROR( "TLS: Unable to open certificate file: %s", certPath.c_str() );
		return false;
	}

	X509* cert = PEM_read_X509( fp, nullptr, nullptr, nullptr );
	fclose( fp );

	if( !cert )
	{
		LOG_ERROR( "TLS: Unable to parse certificate: %s", certPath.c_str() );
		return false;
	}

	const ASN1_TIME* notAfter = X509_get0_notAfter( cert );
	int pday = 0, psec = 0;
	ASN1_TIME_diff( &pday, &psec, nullptr, notAfter );

	bool expired = ( pday < 0 || ( pday == 0 && psec < 0 ) );

	if( expired )
	{
		LOG_ERROR( "TLS WARNING: Certificate has expired! (%s)", certPath.c_str() );
		LOG_ERROR( "TLS WARNING: Server will start but clients may reject the connection." );
	}
	else
	{
		LOG_INFO( "TLS: Certificate expires in %d days (%s)", pday, certPath.c_str() );
		if( pday < 30 )
		{
			LOG_WARNING( "TLS WARNING: Certificate expires in less than 30 days -- consider renewing." );
		}
	}

	X509_free( cert );
	return true;
}
#endif

bool CrowListener::ConfigureSSL()
{
#ifdef CROW_ENABLE_SSL
	if( !m_Secure )
		return true;

	namespace fs = std::filesystem;

	if( m_CertPath.empty() || m_KeyPath.empty() )
	{
		LOG_ERROR( "TLS ERROR: TLS is enabled but certificate paths are not configured." );
		LOG_ERROR( "  Set server_tls_cert and server_tls_key in the database settings," );
		LOG_ERROR( "  or run Setup-TLS.ps1 to configure TLS certificates." );
		LOG_ERROR( "  To disable TLS, set server_tls_mode to NoSecurity." );
		return false;
	}

	if( !fs::exists( m_CertPath ) )
	{
		LOG_ERROR( "TLS ERROR: Certificate file not found: %s", m_CertPath.c_str() );
		return false;
	}

	if( !fs::exists( m_KeyPath ) )
	{
		LOG_ERROR( "TLS ERROR: Private key file not found: %s", m_KeyPath.c_str() );
		return false;
	}

	LogCertExpiry( m_CertPath );

	m_App.ssl_file( m_CertPath, m_KeyPath );

	// Track cert modification time for auto-reload
	std::error_code ec;
	m_LastCertModTime = fs::last_write_time( m_CertPath, ec );

	LOG_INFO( "TLS: Configured with cert=%s key=%s", m_CertPath.c_str(), m_KeyPath.c_str() );
	return true;
#else
	if( m_Secure )
	{
		LOG_ERROR( "TLS ERROR: TLS requested but CROW_ENABLE_SSL is not compiled in." );
		return false;
	}
	return true;
#endif
}

// Resolve a hostname to a bind address. ASIO requires a numeric IP.
static std::string ResolveBindAddress( const std::string& hostname )
{
	if( hostname == "localhost" )
		return "127.0.0.1";
	if( hostname == "+" || hostname == "*" || hostname == "0.0.0.0" || hostname.empty() )
		return "0.0.0.0";

	// Check if it's already a numeric IP
	std::error_code ec;
	asio::ip::make_address( hostname, ec );
	if( !ec )
		return hostname;

	// It's a domain name -- bind to all interfaces
	return "0.0.0.0";
}

bool CrowListener::ReloadTLS()
{
#ifdef CROW_ENABLE_SSL
	if( !m_Secure )
	{
		LOG_INFO( "TLS reload skipped -- TLS is not enabled." );
		return true;
	}

	LOG_INFO( "TLS: Reloading certificate..." );

	LogCertExpiry( m_CertPath );

	// Reload the SSL context on the underlying ASIO ssl_context
	// New connections will use the updated certificate
	try
	{
		auto* sslCtx = SSL_CTX_new( TLS_server_method() );
		if( !sslCtx )
		{
			LOG_ERROR( "TLS reload failed: unable to create new SSL context." );
			return false;
		}

		if( SSL_CTX_use_certificate_chain_file( sslCtx, m_CertPath.c_str() ) != 1 )
		{
			LOG_ERROR( "TLS reload failed: unable to load certificate." );
			SSL_CTX_free( sslCtx );
			return false;
		}

		if( SSL_CTX_use_PrivateKey_file( sslCtx, m_KeyPath.c_str(), SSL_FILETYPE_PEM ) != 1 )
		{
			LOG_ERROR( "TLS reload failed: unable to load private key." );
			SSL_CTX_free( sslCtx );
			return false;
		}

		SSL_CTX_free( sslCtx );

		// Validated successfully -- now do a graceful server restart
		LOG_INFO( "TLS: Certificate validated, restarting server..." );
		m_Ready.store( false, std::memory_order_release );
		m_App.stop();
		if( m_ServerThread.joinable() )
			m_ServerThread.join();

		m_App.ssl_file( m_CertPath, m_KeyPath );

		std::string bindAddr = ResolveBindAddress( m_Hostname );

		m_App.bindaddr( bindAddr ).port( m_Port );
		Witness::HttpDiagnostics::ClearActiveRequests();
		m_ServerThread = std::thread( [this]()
		{
			try
			{
				m_App.loglevel( crow::LogLevel::Warning );
				m_App.concurrency( m_CrowThreadCount ).run();
			}
			catch( const std::exception& e )
			{
				LOG_ERROR( "Server error after TLS reload: %s", e.what() );
			}
			m_Ready.store( false, std::memory_order_release );
		});
		if( m_App.wait_for_server_start( std::chrono::seconds( 10 ) ) !=
			std::cv_status::no_timeout || !m_App.is_bound() )
		{
			LOG_ERROR( "TLS reload did not restore the Crow listener within 10 seconds." );
			return false;
		}
		m_Ready.store( true, std::memory_order_release );

		// Update tracked modification time
		std::error_code ec;
		m_LastCertModTime = std::filesystem::last_write_time( m_CertPath, ec );

		LOG_INFO( "TLS: Certificate reloaded successfully." );
		return true;
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "TLS reload failed: %s", e.what() );
		return false;
	}
#else
	LOG_INFO( "TLS reload skipped -- CROW_ENABLE_SSL not compiled in." );
	return false;
#endif
}

void CrowListener::CertMonitorLoop()
{
#ifdef CROW_ENABLE_SSL
	namespace fs = std::filesystem;

	while( m_CertMonitorRunning.load() )
	{
		// Check every 12 hours
		for( int i = 0; i < 12 * 60 && m_CertMonitorRunning.load(); i++ )
		{
			std::this_thread::sleep_for( std::chrono::minutes(1) );
		}

		if( !m_CertMonitorRunning.load() )
			break;

		std::error_code ec;
		auto currentModTime = fs::last_write_time( m_CertPath, ec );
		if( ec )
			continue;

		if( currentModTime != m_LastCertModTime )
		{
			LOG_INFO( "TLS: Certificate file changed on disk, triggering reload..." );
			ReloadTLS();
		}
	}
#endif
}

bool CrowListener::Start()
{
	m_Ready.store( false, std::memory_order_release );
	if( !ConfigureSSL() )
	{
		LOG_ERROR( "Server failed to start due to TLS configuration error." );
		return false;
	}

	std::string bindAddr = ResolveBindAddress( m_Hostname );

	m_CrowThreadCount = std::max( 4u, std::thread::hardware_concurrency() * 2 );
	LOG_INFO( "Crow HTTP server: %u worker threads", m_CrowThreadCount );

	try
	{
		m_App.bindaddr( bindAddr ).port( m_Port );
		Witness::HttpDiagnostics::ClearActiveRequests();

		m_ServerThread = std::thread( [this]()
		{
			try
			{
				m_App.loglevel( crow::LogLevel::Warning );
				m_App.concurrency( m_CrowThreadCount ).run();
			}
			catch( const std::exception& e )
			{
				LOG_ERROR( "Server error: %s", e.what() );
			}
			m_Ready.store( false, std::memory_order_release );
		});
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Failed to start server on %s:%d -- %s", bindAddr.c_str(), m_Port, e.what() );
		return false;
	}
	// Crow's run() is launched on another thread. Its start notification follows
	// bind, listen, worker setup, and the first accept, so the dashboard must not
	// advertise a ready listener before this point.
	const bool Ready = m_App.wait_for_server_start( std::chrono::seconds( 10 ) ) ==
		std::cv_status::no_timeout && m_App.is_bound();
	if( !Ready )
	{
		LOG_ERROR( "Crow server did not become ready on %s:%d within 10 seconds.",
			bindAddr.c_str(), m_Port );
		return false;
	}
	m_Ready.store( true, std::memory_order_release );

	LOG_INFO( "Crow server ready on %s:%d (%s)", m_Hostname.c_str(), m_Port, m_Secure ? "HTTPS" : "HTTP" );

	// Start cert file monitor if TLS is active
	if( m_Secure && !m_CertPath.empty() )
	{
		m_CertMonitorRunning = true;
		m_CertMonitorThread = std::thread( &CrowListener::CertMonitorLoop, this );
	}
	return true;
}

void CrowListener::Stop()
{
	m_Ready.store( false, std::memory_order_release );
	// Stop cert monitor
	m_CertMonitorRunning = false;
	if( m_CertMonitorThread.joinable() )
		m_CertMonitorThread.join();

	m_App.stop();

	if( m_ServerThread.joinable() )
	{
		m_ServerThread.join();
	}
}
