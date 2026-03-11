#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"
#include "CrowListener.h"

#include <Log.h>
#include <future>
#include <filesystem>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")

void WitnessServer::HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data)
{
	std::shared_ptr<CameraWorker> Worker;
	std::string CameraName;

	{
		auto CameraState = Context->FindCameraById( Data.Camera );
		if(CameraState)
		{
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
			HandleActions( Context, *CameraState, Data.Camera, Data.MotionPercentage );

			//Already recording
			if (CameraState->IsRecording)
			{
				return;
			}

			CameraState->IsRecording = true;

			// Broadcast recording started
			crow::json::wvalue ev;
			ev["cameraID"] = Data.Camera;
			ev["recording"] = true;
			Context->Events->Broadcast( "camera:recording", std::move( ev ) );
		}
	}

	StartCameraRecording( Worker, Data.ClipStats.TimestampClipStarted, Data.Camera, false, Data.Result );
};

void WitnessServer::HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data)
{
	{	
		auto CameraState = Context->FindCameraById( Data.Camera );
		if( CameraState )
		{
			CameraState->ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;

			HandleActions( Context, *CameraState, Data.Camera, Data.ClipStats.LargestMotionDelta );
		}
	}
};

void WitnessServer::HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data)
{
	auto StopRecord = std::make_shared<CameraStopRecordMessage>( Data.Camera, false );

	std::shared_ptr<CameraWorker> Worker;

	{
		auto CameraState = Context->FindCameraById( Data.Camera );
		if( CameraState )
		{
			Worker = CameraState->Worker;

			if (CameraState->IsManualRecording)
			{
				return;
			}

			CameraState->IsRecording = false;

			// Broadcast recording stopped
			crow::json::wvalue ev;
			ev["cameraID"] = Data.Camera;
			ev["recording"] = false;
			Context->Events->Broadcast( "camera:recording", std::move( ev ) );
		}
	}

	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StopRecord );
	}
};

void WitnessServer::HandleActions( const std::shared_ptr<GlobalContext>& Context, CameraState& State, int CameraIndex, double MotionThreshold )
{
	SQLiteDatabaseQueryInstance FindActions( Context->Database, "FindActions" );
	FindActions->Bind( "@CameraUID", CameraIndex );
	FindActions->Bind( "@MDThreshold", (double)MotionThreshold );

	std::string ClipFilename;

	bool Success = false;

	std::vector<int> ActionsToTake;

	struct ActionCommand
	{
		std::string Name;
		std::string Command;
		std::string Param1;
		std::string Param2;
		std::string Param3;
	};
	std::vector<ActionCommand> Commands;

	FindActions->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			int ActionUID = query.GetColumnValueInt(0);

			bool Found = false;
			for (int Action : State.TriggeredActions)
			{
				if (Action == ActionUID)
				{
					Found = true;
				}
			}

			if (!Found)
			{
				ActionsToTake.push_back(ActionUID);
			}
			
			return true;
		}
	);

	for( int Action : ActionsToTake )
	{
		State.TriggeredActions.push_back(Action);

		SQLiteDatabaseQueryInstance GetAction( Context->Database, "GetAction" );
		GetAction->Bind( "@ActionUID", Action );

		GetAction->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				ActionCommand Command;

				//int ActionUID = query.GetColumnValueInt(0);
				//std::string Name = query.GetColumnValueText(1);
				Command.Command = query.GetColumnValueText(2);
				Command.Param1 = query.GetColumnValueText(3);
				Command.Param2 = query.GetColumnValueText(4);
				Command.Param3 = query.GetColumnValueText(5);

				Commands.push_back(Command);
						
				return true;
			}
		);
	}

	for (auto& Command : Commands)
	{
		TriggerAction( Command.Command, Command.Param1, Command.Param2, Command.Param3, State, CameraIndex );
	}
}

void WitnessServer::TriggerAction( const std::string& Command, const std::string& Param1, const std::string& Param2, const std::string& Param3, CameraState& State, int CameraIndex )
{
	if (Command.compare("PlaySound") == 0)
	{
		// Resolve relative paths against exe directory
		std::filesystem::path soundPath( Param1 );
		if( soundPath.is_relative() )
		{
			wchar_t exeBuf[MAX_PATH] = {};
			GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
			soundPath = std::filesystem::path( exeBuf ).parent_path() / soundPath;
		}
		std::string soundFile = soundPath.string();
		std::async([soundFile]() {
			PlaySoundA( soundFile.c_str(), nullptr, SND_FILENAME | SND_ASYNC );
		});
	}
	else
	{
		LOG_WARNING( "Unknown command: %s", Command.c_str() );
	}
}