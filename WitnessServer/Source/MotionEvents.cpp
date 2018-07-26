#include "Witness.h"
#include "CameraWorker.h"
#include "GlobalContext.h"
#include "Listener.h"
#include "Android/AndroidNotify.h"
#include <windows.h>

#include <future>
#pragma comment(lib, "Winmm.lib")

void WitnessServer::HandleCameraBeginMotionMessage(const CameraBeginMotionMessage& Data)
{
	shared_ptr<CameraWorker> Worker;
	string_t CameraName;

	{
		lock_guard<mutex> Lock( Context->Mutex );
			
		auto Iter = Context->Cameras.find( Data.Camera );
		if( Iter != Context->Cameras.end() )
		{
			auto& CameraState = (*Iter).second;

			if( Data.Jpeg.size() )
			{
				CameraState.ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;
			}
			else
			{
				tcerr << _T("Clip thumbnail is empty") << endl;
			}
			CameraName = CameraState.Name;
			Worker = CameraState.Worker;

			CameraState.TriggeredActions.clear();
			HandleActions( Context, CameraState, Data.Camera, Data.MotionPercentage );

			//Already recording
			if (CameraState.IsRecording)
			{
				return;
			}

			CameraState.IsRecording = true;
		}
	}

	stringstream_t Message;
	Message << _T("Begin Motion: ") << Data.MotionPercentage;

	StatusMessage( Data.Camera, _T(""), Message.str() );

    stringstream_t ThumbPath;
    ThumbPath << Server->GetBaseUri() << _T("clip/thumb/") << Data.Camera << _T("/") << Data.ClipStats.TimestampClipStarted;
						
	SendAndroidNotification( Android.ServerKey, Android.TempUserId, _T("Camera info"), CameraName, ThumbPath.str(), nullptr );
						
	StartCameraRecording( Worker, Data.ClipStats.TimestampClipStarted, Data.Camera, false );
};

void WitnessServer::HandleCameraUpdateMotionMessage(const CameraUpdateMotionMessage& Data)
{
	{
		lock_guard<mutex> Lock( Context->Mutex );
			
		auto Iter = Context->Cameras.find( Data.Camera );
		if( Iter != Context->Cameras.end() )
		{
			auto& CameraState = (*Iter).second;

			CameraState.ClipThumbnails[ Data.ClipStats.TimestampClipStarted ] = Data.Jpeg;

			HandleActions( Context, CameraState, Data.Camera, Data.ClipStats.LargestMotionDelta );
		}
	}
};

void WitnessServer::HandleCameraEndMotionMessage(const CameraEndMotionMessage& Data)
{
	StatusMessage( Data.Camera, _T(""), _T("End Motion") );

	auto StopRecord = make_shared<CameraStopRecordMessage>( Data.Camera );

	shared_ptr<CameraWorker> Worker;

	{
		lock_guard<mutex> Lock( Context->Mutex );

		auto Iter = Context->Cameras.find( Data.Camera );
		if( Iter != Context->Cameras.end() )
		{
			Worker = (*Iter).second.Worker;

			if ((*Iter).second.IsManualRecording)
			{
				return;
			}

			(*Iter).second.IsRecording = false;
		}
	}

	if( Worker )
	{
		Context->MessageBus->SendToClient( Worker.get(), StopRecord );
	}
};

void WitnessServer::HandleActions( const shared_ptr<GlobalContext>& Context, CameraState& State, int CameraIndex, double MotionThreshold )
{
	SQLiteDatabaseQueryInstance FindActions( Context->Database, _T("FindActions") );
	FindActions->Bind( "@CameraUID", CameraIndex );
	FindActions->Bind( "@MDThreshold", (double)MotionThreshold );

	string_t ClipFilename;

	bool Success = false;

	vector<int> ActionsToTake;

	struct ActionCommand
	{
		string_t Name;
		string_t Command;
		string_t Param1;
		string_t Param2;
		string_t Param3;
	};
	vector<ActionCommand> Commands;

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
				//string_t Name = query.GetColumnValueText(1);
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

void WitnessServer::TriggerAction( const string_t& Command, const string_t& Param1, const string_t& Param2, const string_t& Param3, CameraState& State, int CameraIndex )
{
	if (Command.compare(_T("PlaySound")) == 0)
	{
		std::async([=]() {
			PlaySound( Param1.c_str(), nullptr, SND_FILENAME );
		});
	}
	else
	{
		tcerr << _T("Unknown command: ") << Command << endl;
	}
}