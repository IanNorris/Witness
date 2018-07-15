#include "CameraWorker.h"
#include "ObservingMotionFilter.h"
#include "PersonRecognitionFilter.h"

#include <windows.h>

void CameraWorker::CreateInputStream()
{
	InputStreamSetup Setup;
	Setup.GetTimestamp = datetime::utc_timestamp;
	Setup.MotionFilterFrameSkip = Camera.SkipFrames;
	Setup.MotionDetectFrameHeight = Camera.MDFrameHeight;
	Setup.MotionDetectThreshold = Camera.MDThreshold;
	Setup.HistoricalPacketBufferSeconds = Video.ClipHistoryPeriod;

	CameraStream = make_shared<InputStream>( Setup, Camera.ID, Camera.JobQueue, std::string( Camera.Path.begin(), Camera.Path.end() ) );
}

void CameraWorker::WorkerInit()
{
	Filter = make_shared<ObservingMotionFilter>( Camera.MDThreshold, std::string( Camera.MotionFilterName.begin(), Camera.MotionFilterName.end() ).c_str(), Camera.ID, MessageBusObject );

	auto PersonFilter = make_shared<PersonRecognitionFilter>(
		std::string( Camera.FaceCascadeFilter.begin(), Camera.FaceCascadeFilter.end() ).c_str(),
		std::string( Camera.FullBodyCascadeFilter.begin(), Camera.FullBodyCascadeFilter.end() ).c_str(),
		"FaceRecognition"
		);

	Filter->AddChildFilter( dynamic_pointer_cast<IRecordFilter>(PersonFilter) );

	UpdateLastTimedAction(_T("Startup..."));

	MessageBusObject->SendToClient( nullptr, make_shared<CameraStartupMessage>( Camera.ID ) );

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

	double FrameRate = CameraStream->GetFramerateDouble();
	double FrameTime = 1.0f / FrameRate;

	const double BufferPeriodInMilliseconds = 2.0;
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;
	uint64_t Start = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	if (LastFrameTime > 0)
	{
		double Duration = (double)(Start - LastFrameTime) / NanoSecondsToSeconds;
		if (Duration < FrameTime)
		{
			double MillisecondsToWait = ((FrameTime - Duration) * 1000.0);

			MillisecondsToWait = min( MillisecondsToWait - BufferPeriodInMilliseconds, 0 );

			if( MillisecondsToWait > 0.0 )
			{
				Sleep( (DWORD)MillisecondsToWait );
			}
		}
	}
	
	CameraStreamError Error = CameraStream->ProcessFrame( static_pointer_cast<IRecordFilter>(Filter), RecordStream.get() );

	LastFrameTime = Start;

	if( Error == CameraStreamError::Success )
	{
		if( !IsConnected )
		{
			IsConnected = true;

			MessageBusObject->SendToClient( nullptr, make_shared<CameraConnectedMessage>( Camera.ID ) );
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

		MessageBusObject->SendToClient( nullptr, make_shared<CameraReconnectMessage>( Camera.ID, ErrorStr ) );

		CreateInputStream();

		Camera.JobQueue->RemoveAllForSource( Camera.ID );

		Sleep( 3000 );
	}
}

void CameraWorker::OnClipFinished()
{
	if (RecordStream)
	{
		Filter->SetManualClipEnd( datetime::utc_timestamp() );

		auto FinishedMessage = make_shared<CameraClipFinishedMessage>( Camera.ID );
		FinishedMessage->ClipStats = Filter->GetClipStatistics();
		MessageBusObject->SendToClient( nullptr, FinishedMessage );
		Filter->ClearStats();

		RecordStream->CloseFile();
		RecordStream.reset();
	}
}
