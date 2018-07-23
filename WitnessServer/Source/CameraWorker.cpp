#include "CameraWorker.h"
#include "MotionVectorFilter.h"
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
	Setup.ExportMotionVectors = Video.ExportMotionVectors != 0;

	std::string CamPath = std::string( Camera.Path.begin(), Camera.Path.end() );

	CameraStream = make_shared<InputStream>( Setup, Camera.ID, Camera.JobQueue, CamPath );

	if (_strnicmp(CamPath.c_str(), "rtsp://", 7) == 0)
	{
		IsRTSP = true;
	}
}

void CameraWorker::WorkerInit()
{
	UpdateLastTimedAction(_T("Creating filters..."));

	auto RootMotionNode = make_shared<MotionChainNode>();
	RootMotionNode->Filter = make_shared<MotionVectorFilter>();
	RootMotionNode->MinimumThreshold = (float)Camera.MDThreshold;
	RootMotionNode->InclusiveFilter = ClassificationResult::Motion_Motion;
	RootMotionNode->ExclusiveFilter = 0;

	/*auto SecondPassMotionNode = make_shared<MotionChainNode>();
	RootMotionNode->OnSuccess = SecondPassMotionNode;
	SecondPassMotionNode->Filter = make_shared<MotionFilter>( std::string( Camera.MotionFilterName.begin(), Camera.MotionFilterName.end() ).c_str() );
	SecondPassMotionNode->MinimumThreshold = (float)Camera.MDThreshold;
	SecondPassMotionNode->InclusiveFilter = ClassificationResult::Motion_Motion;
	SecondPassMotionNode->ExclusiveFilter = 0;*/

	auto PersonMotionNode = make_shared<MotionChainNode>();
	RootMotionNode->OnSuccess = PersonMotionNode;
	//SecondPassMotionNode->OnSuccess = PersonMotionNode;
	PersonMotionNode->Filter = make_shared<PersonRecognitionFilter>(
		std::string( Camera.FaceCascadeFilter.begin(), Camera.FaceCascadeFilter.end() ).c_str(),
		std::string( Camera.FullBodyCascadeFilter.begin(), Camera.FullBodyCascadeFilter.end() ).c_str()
	);
	PersonMotionNode->InclusiveFilter = ClassificationResult::Motion_Person;
	PersonMotionNode->ExclusiveFilter = 0;
	PersonMotionNode->MinimumThreshold = 0.0f;

	Filter = make_shared<ObservingMotionFilter>( RootMotionNode, Camera.ID, MessageBusObject );

	MessageBusObject->SendToClient( nullptr, make_shared<CameraStartupMessage>( Camera.ID ) );

	UpdateLastTimedAction(_T("Starting camera connection..."));

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

	const double BufferPeriodInMilliseconds = 0.0;
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;
	uint64_t Start = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	if (LastFrameTime > 0 && !IsRTSP)
	{
		double Duration = (double)(Start - LastFrameTime) / NanoSecondsToSeconds;
		if (Duration < FrameTime)
		{
			double MillisecondsToWait = ((FrameTime - Duration) * 1000.0);

			MillisecondsToWait = max( MillisecondsToWait - BufferPeriodInMilliseconds, 0 );

			if( MillisecondsToWait > 0.0  )
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
		LastFrameTime = 0;

		Camera.JobQueue->RemoveAllForSource( Camera.ID );

		if( Error != CameraStreamError::EndOfFile )
		{
			Sleep( 3000 );
		}
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
