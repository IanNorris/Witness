#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"
#include "CrowListener.h"
#include "SoundManager.h"
#include "SQLite.h"
#include <Log.h>

static void BroadcastMotionState( GlobalContext& Context, int CameraID, bool Active )
{
	crow::json::wvalue Event;
	Event["cameraID"] = CameraID;
	Event["active"] = Active;
	Context.Events->Broadcast( "camera:motion", std::move( Event ) );
}

void WitnessServer::HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data)
{
	SetOperationalActivity( Data.Camera, true, true );
	std::shared_ptr<CameraWorker> Worker;
	std::string CameraName;
	bool StartRecording = false;

	{
		auto CameraState = Context->FindCameraById( Data.Camera );
		if(CameraState)
		{
			CameraState->IsMotionActive = true;
			BroadcastMotionState( *Context, Data.Camera, true );
			if( Data.Jpeg.size() )
			{
				CameraState->ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
			}
			else
			{
				LOG_WARNING( "Clip thumbnail is empty" );
			}
			CameraName = CameraState->Name;
			Worker = CameraState->Worker;

			CameraState->TriggeredActions.clear();

			// Trigger generic motion actions (DetectionClass = "")
			if( Context->Sound )
			{
				SQLiteDatabaseQueryInstance findQ( Context->Database, "FindActions" );
				findQ->Bind( "@CameraUID", Data.Camera );
				findQ->Bind( "@MDThreshold", Data.MotionPercentage );

				std::vector<int> actionUIDs;
				findQ->Execute( [&]( const SQLiteDatabaseQuery& q ) {
					actionUIDs.push_back( q.GetColumnValueInt( 0 ) );
					return true;
				});

				for( int uid : actionUIDs )
				{
					SQLiteDatabaseQueryInstance getQ( Context->Database, "GetAction" );
					getQ->Bind( "@ActionUID", uid );
					getQ->Execute( [&]( const SQLiteDatabaseQuery& q ) {
						std::string command = q.GetColumnValueText( 2 );
						std::string param1  = q.GetColumnValueText( 3 );
						int priority        = q.GetColumnValueInt( 6 );
						int cooldown        = q.GetColumnValueInt( 7 );

						if( command == "PlaySound" )
						{
							auto soundFile = SoundManager::ResolveSoundPath( param1 );
							if( Context->Sound->TryPlaySound( uid, priority, cooldown, soundFile ) )
							{
								LOG_INFO( "Motion action on camera %d -> PlaySound(%s) pri=%d cd=%ds",
									Data.Camera, soundFile.c_str(), priority, cooldown );
							}
						}
						return true;
					});
				}
			}

			if (!CameraState->IsRecording)
			{
				CameraState->IsRecording = true;
				StartRecording = true;

				// Broadcast recording started
				crow::json::wvalue ev;
				ev["cameraID"] = Data.Camera;
				ev["recording"] = true;
				Context->Events->Broadcast( "camera:recording", std::move( ev ) );
			}
		}
	}

	if( StartRecording )
		StartCameraRecording( Worker, Data.ClipStats.TimestampClipStarted, Data.Camera, false, Data.Result );

	// Trigger recording on cameras that use this camera as their motion source
	if (Context->Database)
	{
		SQLiteDatabaseQueryInstance findPaired(Context->Database, "GetCamerasWithMotionSource");
		findPaired->Bind("@SourceCameraId", Data.Camera);

		std::vector<int> pairedCameraIds;
		findPaired->Execute([&](const SQLiteDatabaseQuery& q) {
			pairedCameraIds.push_back(q.GetColumnValueInt(0));
			return true;
		});

		for (int pairedId : pairedCameraIds)
		{
			SetOperationalActivity( pairedId, true, true );
			auto pairedState = Context->FindCameraById(pairedId);
			if (pairedState)
			{
				pairedState->IsMotionActive = true;
				BroadcastMotionState( *Context, pairedId, true );
			}
			if (pairedState && !pairedState->IsRecording && !pairedState->IsManualRecording)
			{
				pairedState->IsRecording = true;

				crow::json::wvalue pairedEv;
				pairedEv["cameraID"] = pairedId;
				pairedEv["recording"] = true;
				Context->Events->Broadcast("camera:recording", std::move(pairedEv));

				StartCameraRecording(pairedState->Worker, Data.ClipStats.TimestampClipStarted, pairedId, false, Data.Result);
				LOG_INFO("Motion on camera %d triggered paired camera %d", Data.Camera, pairedId);
			}
		}
	}
};

void WitnessServer::HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data)
{
	{	
		auto CameraState = Context->FindCameraById( Data.Camera );
		if( CameraState )
		{
			CameraState->ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
		}
	}
};

void WitnessServer::HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data)
{
	SetOperationalMotion( Data.Camera, false );
	auto StopRecord = std::make_shared<CameraStopRecordMessage>( Data.Camera, false );

	std::shared_ptr<CameraWorker> Worker;
	bool StopRecording = false;

	{
		auto CameraState = Context->FindCameraById( Data.Camera );
		if( CameraState )
		{
			Worker = CameraState->Worker;
			CameraState->IsMotionActive = false;
			BroadcastMotionState( *Context, Data.Camera, false );

			if (!CameraState->IsManualRecording)
			{
				CameraState->IsRecording = false;
				StopRecording = true;

				// Broadcast recording stopped
				crow::json::wvalue ev;
				ev["cameraID"] = Data.Camera;
				ev["recording"] = false;
				Context->Events->Broadcast( "camera:recording", std::move( ev ) );
			}
		}
	}

	if( StopRecording && Worker )
	{
		SetOperationalRecording( Data.Camera, false );
		Context->MessageBus->SendToClient( Worker.get(), StopRecord );
	}

	// Stop recording on cameras that use this camera as their motion source
	if (Context->Database)
	{
		SQLiteDatabaseQueryInstance findPaired(Context->Database, "GetCamerasWithMotionSource");
		findPaired->Bind("@SourceCameraId", Data.Camera);

		std::vector<int> pairedCameraIds;
		findPaired->Execute([&](const SQLiteDatabaseQuery& q) {
			pairedCameraIds.push_back(q.GetColumnValueInt(0));
			return true;
		});

		for (int pairedId : pairedCameraIds)
		{
			SetOperationalMotion( pairedId, false );
			auto pairedState = Context->FindCameraById(pairedId);
			if (pairedState)
			{
				pairedState->IsMotionActive = false;
				BroadcastMotionState( *Context, pairedId, false );
			}
			if (pairedState && pairedState->IsRecording && !pairedState->IsManualRecording)
			{
				SetOperationalRecording( pairedId, false );
				pairedState->IsRecording = false;

				crow::json::wvalue pairedEv;
				pairedEv["cameraID"] = pairedId;
				pairedEv["recording"] = false;
				Context->Events->Broadcast("camera:recording", std::move(pairedEv));

				auto pairedStop = std::make_shared<CameraStopRecordMessage>(pairedId, false);
				if (pairedState->Worker)
				{
					Context->MessageBus->SendToClient(pairedState->Worker.get(), pairedStop);
				}
			}
		}
	}
};
