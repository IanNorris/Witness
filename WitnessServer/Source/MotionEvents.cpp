#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"
#include "CrowListener.h"

#include <future>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")

void WitnessServer::HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data)
{
	std::shared_ptr<CameraWorker> Worker;
	StringT CameraName;

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
				std::tcerr << _T("Clip thumbnail is empty") << std::endl;
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
		}
	}

	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StopRecord );
	}
};

void WitnessServer::HandleActions( const std::shared_ptr<GlobalContext>& Context, CameraState& State, int CameraIndex, double MotionThreshold )
{
	SQLiteDatabaseQueryInstance FindActions( Context->Database, _T("FindActions") );
	FindActions->Bind( "@CameraUID", CameraIndex );
	FindActions->Bind( "@MDThreshold", (double)MotionThreshold );

	StringT ClipFilename;

	bool Success = false;

	std::vector<int> ActionsToTake;

	struct ActionCommand
	{
		StringT Name;
		StringT Command;
		StringT Param1;
		StringT Param2;
		StringT Param3;
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

		SQLiteDatabaseQueryInstance GetAction( Context->Database, _T("GetAction") );
		GetAction->Bind( "@ActionUID", Action );

		GetAction->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				ActionCommand Command;

				//int ActionUID = query.GetColumnValueInt(0);
				//StringT Name = query.GetColumnValueText(1);
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

void WitnessServer::TriggerAction( const StringT& Command, const StringT& Param1, const StringT& Param2, const StringT& Param3, CameraState& State, int CameraIndex )
{
	if (Command.compare(_T("PlaySound")) == 0)
	{
		std::async([=]() {
			PlaySound( Param1.c_str(), nullptr, SND_FILENAME | SND_ASYNC );
		});
	}
	else
	{
		std::tcerr << _T("Unknown command: ") << Command << std::endl;
	}
}