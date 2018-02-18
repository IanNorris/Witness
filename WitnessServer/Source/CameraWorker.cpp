#include "CameraWorker.h"
#include "ObservingMotionFilter.h"

#include <windows.h>

void CameraWorker::WorkerThread()
{
	MessageBusQueue = MessageBus->AddClient( this );
	Filter = make_shared<ObservingMotionFilter>( CameraID, MessageBus );
	CameraStream = make_shared<InputStream>( std::string( Path.begin(), Path.end() ) );

	auto& MB = *MessageBus;

	MessageBus->SendToClient( nullptr, make_shared<CameraStartupMessage>( CameraID ) );

	auto OnClipFinished = [&]()
	{
		if (RecordStream)
		{
			Filter->SetManualClipEnd( datetime::utc_timestamp() );

			auto FinishedMessage = make_shared<CameraClipFinishedMessage>( CameraID );
			FinishedMessage->ClipStats = Filter->GetClipStatistics();
			MB.SendToClient( nullptr, FinishedMessage );
			Filter->ClearStats();

			RecordStream->CloseFile();
			RecordStream.reset();
		}
	};

	while( !Shutdown )
	{
		shared_ptr<Message> Msg;
		if( MessageBusQueue->TryPop( Msg ) )
		{
			Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
			{
				Shutdown = true;
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

			MessageBus->SendToClient( nullptr, make_shared<CameraReconnectMessage>( CameraID, GetCameraStreamErrorMessage(Error) ) );

			CameraStream = make_shared<InputStream>( std::string( Path.begin(), Path.end() ) );
		}
	}

	//Ensure destruction is done on the worker thread
	Filter = nullptr;
	CameraStream = nullptr;

	MessageBus->SendToClient( nullptr, make_shared<ThreadShutdownMessage>() );

	MessageBus->RemoveClient( this );
	MessageBusQueue = nullptr;

	//Don't allow Complete to be fired until all work above is complete
	MemoryBarrier();

	Complete = true;
}

void CameraWorker::RequestShutdown()
{
	Shutdown = true;

	MemoryBarrier();
}
