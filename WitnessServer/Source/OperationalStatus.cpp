#include "Witness.h"
#include "CrowListener.h"
#include "GlobalContext.h"
#include "BuildIdentity.h"
#include "CpuUsage.h"

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

OperationalStatus WitnessServer::GetOperationalStatus() const
{
	OperationalStatus Result;
	if( !Context )
		return Result;
	Result.WebReady = Server && Server->IsReady();

#ifdef _WIN32
	Result.HostCpuPercent = Witness::Diagnostics::SampleCpuUsage().HostPercent.value_or(-1.0);
	MEMORYSTATUSEX Memory{};
	Memory.dwLength = sizeof( Memory );
	if( GlobalMemoryStatusEx( &Memory ) )
	{
		Result.HostTotalMemoryBytes = Memory.ullTotalPhys;
		Result.HostAvailableMemoryBytes = Memory.ullAvailPhys;
	}
	PROCESS_MEMORY_COUNTERS_EX ProcessMemory{};
	if( GetProcessMemoryInfo( GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &ProcessMemory ), sizeof( ProcessMemory ) ) )
	{
		Result.ProcessWorkingSetBytes = ProcessMemory.WorkingSetSize;
		Result.ProcessPrivateBytes = ProcessMemory.PrivateUsage;
	}
#endif

	struct CameraCopy
	{
		int Id;
		std::string Name;
		std::shared_ptr<CameraWorker> Worker;
	};

	std::vector<CameraCopy> Copies;
	{
		std::shared_lock<std::shared_mutex> lock( Context->Mutex );
		Result.BuildHash = Context->BuildHash;
        Result.NativeBuildHash = GetNativeBuildIdentity();
        Result.CameraBuildHash = Witness::Camera::LiveOutputStream::GetNativeBuildIdentity();
        Result.NativeBuildMismatch = Result.NativeBuildHash != Result.CameraBuildHash ||
            std::string(GetNativeBuildConfiguration()) != Witness::Camera::LiveOutputStream::GetNativeBuildConfiguration();
		Result.Port = Context->Port;
		Copies.reserve( Context->GetCameraMap().size() );
		for( const auto& [Id, State] : Context->GetCameraMap() )
		{
			Copies.push_back( { Id, State.Name, State.Worker } );
		}
	}

	std::sort( Copies.begin(), Copies.end(), []( const CameraCopy& Left, const CameraCopy& Right )
	{
		return Left.Id < Right.Id;
	} );

	Result.Cameras.reserve( Copies.size() );
	for( const auto& Copy : Copies )
	{
		OperationalCameraStatus Camera;
		Camera.Id = Copy.Id;
		Camera.Name = Copy.Name;
		{
			std::lock_guard<std::mutex> lock( OperationalMutex );
			auto Flags = OperationalCameraStates.find( Copy.Id );
			if( Flags != OperationalCameraStates.end() )
			{
				Camera.State = Flags->second.State;
				Camera.Recording = Flags->second.Recording;
				Camera.MotionActive = Flags->second.MotionActive;
			}
			else
				Camera.State = "Starting";
		}

		if( Context->CommonImageProcessingJobQueue )
		{
			auto Stats = Context->CommonImageProcessingJobQueue->GetStats( Copy.Id );
			Camera.PendingEssential = Stats.PendingEssentialJobs;
			Camera.PendingAI = Stats.PendingAIJobs;
			Camera.OldestEssentialMs = (double)Stats.OldestPendingEssentialAgeNS / 1000000.0;
			Camera.OldestAIMs = (double)Stats.OldestPendingAIAgeNS / 1000000.0;
		}

		if( Copy.Worker )
		{
			auto Live = Copy.Worker->GetLiveStream();
			if( Live )
			{
				auto Diag = Live->GetStreamingDiagnostics( false );
				Camera.MainAvailable = true;
				Camera.MainEstablished = Diag.StreamEstablished;
				Camera.Codec = Diag.VideoCodec;
				Camera.Width = Diag.VideoWidth;
				Camera.Height = Diag.VideoHeight;
				Camera.RetainedSegments = Diag.BacklogSize;
				// Init generation 1 is the first successful connection, not a reconnect.
				Camera.Reconnects = std::max( 0, Diag.ReconnectCount - 1 );
				Camera.DroppedPackets = Diag.EstablishedDroppedVideoPackets;
				Camera.RepairedTimestamps = Diag.EstablishedRepairedVideoTimestamps;
				Camera.CorruptPackets = Diag.CorruptVideoPackets;
			}

			auto Preview = Copy.Worker->GetSubStreamWorker();
			Camera.PreviewConnected = Preview && Preview->IsConnected();
		}

		Result.Cameras.push_back( std::move( Camera ) );
	}

	return Result;
}

void WitnessServer::SetOperationalState( int Camera, const std::string& State )
{
	std::lock_guard<std::mutex> lock( OperationalMutex );
	OperationalCameraStates[Camera].State = State;
}

void WitnessServer::SetOperationalActivity( int Camera, bool Recording, bool MotionActive )
{
	std::lock_guard<std::mutex> lock( OperationalMutex );
	auto& Flags = OperationalCameraStates[Camera];
	Flags.Recording = Recording;
	Flags.MotionActive = MotionActive;
}

void WitnessServer::SetOperationalRecording( int Camera, bool Recording )
{
	std::lock_guard<std::mutex> lock( OperationalMutex );
	OperationalCameraStates[Camera].Recording = Recording;
}

void WitnessServer::SetOperationalMotion( int Camera, bool MotionActive )
{
	std::lock_guard<std::mutex> lock( OperationalMutex );
	OperationalCameraStates[Camera].MotionActive = MotionActive;
}
