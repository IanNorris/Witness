#include "CameraWorker.h"
#include "ObservingMotionFilter.h"

#include <windows.h>

void CameraWorker::CreateInputStream()
{
	InputStreamSetup Setup;
	Setup.GetTimestamp = datetime::utc_timestamp;
	Setup.MotionFilterFrameSkip = SkipFrames;
	Setup.MotionDetectFrameHeight = MotionDetectFrameHeight;
	Setup.MotionDetectThreshold = MotionDetectThreshold;

	CameraStream = make_shared<InputStream>( Setup, CameraID, JobQueue, std::string( Path.begin(), Path.end() ) );
}

void CameraWorker::WorkerInit()
{
	Filter = make_shared<ObservingMotionFilter>( MotionDetectThreshold, CameraID, MessageBusObject );

	UpdateLastTimedAction(_T("Startup..."));

	MessageBusObject->SendToClient( nullptr, make_shared<CameraStartupMessage>( CameraID ) );

	CreateInputStream();
}

void CameraWorker::WorkerShutdown()
{
	//Ensure destruction is done on the worker thread
	Filter = nullptr;
	CameraStream = nullptr;

	MessageBusObject->SendToClient( nullptr, make_shared<ThreadShutdownMessage>() );
}

void CameraWorker::WorkerMain()
{
	UpdateLastTimedAction(_T("Work..."));

	shared_ptr<Message> Msg;
	if( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
		{
			RequestShutdown();
		});

		Msg->Handle<CameraStartRecordMessage>([&](const CameraStartRecordMessage& Data)
		{
			OnClipFinished();
				
			Filter->SetManualClipStart( Data.Timestamp );
			RecordStream = make_shared<OutputStream>( string( Data.Path.begin(), Data.Path.end() ), CameraStream.get() );
			RecordStream->Initialize();
		});

		Msg->Handle<CameraStopRecordMessage>([&](const CameraStopRecordMessage& Data)
		{
			OnClipFinished();
		});
	}

	if (IsConnected)
	{
		UpdateLastTimedAction(_T("Processing..."));
	}
	else
	{
		UpdateLastTimedAction(_T("Connecting..."));
	}

	CameraStreamError Error = CameraStream->ProcessFrame( static_pointer_cast<IRecordFilter>(Filter), RecordStream.get() );
	if( Error == CameraStreamError::Success )
	{
		if( !IsConnected )
		{
			IsConnected = true;

			MessageBusObject->SendToClient( nullptr, make_shared<CameraConnectedMessage>( CameraID ) );
		}
	}
	else
	{
		IsConnected = false;

		OnClipFinished();

		string_t ErrorStr = GetCameraStreamErrorMessage(Error);
		if( CameraStream->GetFFMPEGErrorMessage()[0] != '\0')
		{
			string FFMPEGError = CameraStream->GetFFMPEGErrorMessage();
			string_t FFMPEGErrorT = string_t(FFMPEGError.begin(), FFMPEGError.end());
			ErrorStr += _T(": ");
			ErrorStr += FFMPEGErrorT;
		}

		MessageBusObject->SendToClient( nullptr, make_shared<CameraReconnectMessage>( CameraID, ErrorStr ) );

		CreateInputStream();

		JobQueue->RemoveAllForSource( CameraID );

		Sleep( 3000 );
	}
}

void CameraWorker::OnClipFinished()
{
	if (RecordStream)
	{
		Filter->SetManualClipEnd( datetime::utc_timestamp() );

		auto FinishedMessage = make_shared<CameraClipFinishedMessage>( CameraID );
		FinishedMessage->ClipStats = Filter->GetClipStatistics();
		MessageBusObject->SendToClient( nullptr, FinishedMessage );
		Filter->ClearStats();

		RecordStream->CloseFile();
		RecordStream.reset();
	}
}
