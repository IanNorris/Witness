#include "CameraWorker.h"
#include "ObservingMotionFilter.h"

void CameraWorker::WorkerInit()
{
	Filter = make_shared<ObservingMotionFilter>( CameraID, MessageBusObject );

	UpdateLastTimedAction(_T("Connecting..."));

	CameraStream = make_shared<InputStream>( std::string( Path.begin(), Path.end() ) );

	UpdateLastTimedAction(_T("Connected..."));

	MessageBusObject->SendToClient( nullptr, make_shared<CameraStartupMessage>( CameraID ) );
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

	CameraStreamError Error = CameraStream->ProcessFrame( Filter.get(), RecordStream.get() );
	if( Error != CameraStreamError::Success )
	{
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

		CameraStream = make_shared<InputStream>( std::string( Path.begin(), Path.end() ) );
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
