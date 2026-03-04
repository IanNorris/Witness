#include "CameraWorker.h"
#include "GlobalContext.h"
#include "MotionVectorFilter.h"
#include "ObservingMotionFilter.h"
#include "PersonRecognitionFilter.h"
#include "ONNXDetectionFilter.h"
#include "SQLite.h"
#include "EventBroadcaster.h"
#include "crow/json.h"

#include <Log.h>
#include <chrono>
#include <thread>
#include <filesystem>

void CameraWorker::CreateInputStream()
{
	InputStreamSetup Setup;
	Setup.GetTimestamp = GetUnixTimestamp;
	Setup.MotionFilterFrameSkip = Camera.SkipFrames;
	Setup.MotionDetectFrameHeight = Camera.MDFrameHeight;
	Setup.MotionDetectThreshold = Camera.MDThreshold;
	Setup.HistoricalPacketBufferSeconds = Video.ClipHistoryPeriod;
	Setup.ExportMotionVectors = Video.ExportMotionVectors != 0;

	std::string CamPath = Camera.Path;

	std::string CachePath = std::string(Context->CachePath.begin(), Context->CachePath.end());

	CameraStream = std::make_shared<InputStream>( Setup, Camera.ID, Camera.JobQueue, CamPath );

	if (LiveStream)
	{
		LiveStream->ResetForReconnect(CameraStream.get());
	}
	else
	{
		LiveStream = std::make_shared<LiveOutputStream>(CachePath, CameraStream.get(), 1);
	}

	// Continuous recording
	if (Camera.ContinuousRecording)
	{
		std::string continuousPath = (std::filesystem::path(CachePath) / "continuous" / std::to_string(Camera.ID)).string();

		if (ContinuousStream)
		{
			ContinuousStream->ResetForReconnect(CameraStream.get());
		}
		else
		{
			ContinuousStream = std::make_shared<ContinuousOutputStream>(continuousPath, Camera.ID, CameraStream.get());
			ContinuousStream->SetSegmentCompleteCallback(
				[this](int cameraUID, int64_t startTimestamp, int64_t endTimestamp, int duration, const std::string& filePath)
				{
					// Register completed segment in database
					SQLiteDatabaseQueryInstance query(Context->Database, "CreateContinuousSegment");
					query->Bind("@CameraUID", cameraUID);
					query->Bind("@StartTimestamp", startTimestamp);
					query->Bind("@EndTimestamp", endTimestamp);
					query->Bind("@Duration", duration);
					query->Bind("@FilePath", filePath.c_str());
					query->Execute([](const SQLiteDatabaseQuery&) { return true; });

					LOG_INFO("Continuous segment registered: camera %d, %ds, %s", cameraUID, duration, filePath.c_str());

					// Broadcast to WebSocket clients
					crow::json::wvalue ev;
					ev["cameraId"] = cameraUID;
					ev["from"] = startTimestamp;
					ev["to"] = endTimestamp;
					ev["duration"] = duration;
					Context->Events->Broadcast("dvr:segment", std::move(ev));
				}
			);
		}

		// Wire packet callback so ContinuousStream receives every video packet
		auto contStream = ContinuousStream;
		CameraStream->SetPacketCallback([contStream](const AVPacket* pkt)
		{
			contStream->WritePacket(pkt);
		});
	}

	if (_strnicmp(CamPath.c_str(), "rtsp://", 7) == 0)
	{
		IsRTSP = true;
	}
}

void CameraWorker::WorkerInit()
{
	UpdateLastTimedAction("Creating filters...");

	MotionChainNode NoContinuation;
	
	Observer = std::make_shared<ObservingMotionFilter>( NoContinuation, Camera.ID, MessageBusObject );

	MotionChainNode Observing;
	Observing.OnSuccess = Observer;
	Observing.OnFailure = Observer;

	// If ONNX detection is enabled, insert it between motion detection and observer.
	// ONNX receives ALL frames: motion frames for detection, non-motion frames for baseline capture.
	std::shared_ptr<IRecordFilter> PostMotionTarget = Observer;
	std::shared_ptr<IRecordFilter> NoMotionTarget = Observer;

	if( Video.DetectionEnabled && !Video.DetectionModelPath.empty() )
	{
		MotionChainNode DetectionChain;
		DetectionChain.OnSuccess = Observer;
		DetectionChain.OnFailure = Observer;

		auto DetectionFilter = std::make_shared<ONNXDetectionFilter>(
			DetectionChain,
			Video.DetectionModelPath.c_str(),
			(float)Video.DetectionConfidence,
			Video.DetectionUseGPU,
			(float)Video.DetectionMaxFPS,
			Video.DetectionCudnnPath.empty() ? nullptr : Video.DetectionCudnnPath.c_str()
		);

		if( DetectionFilter->IsModelLoaded() )
		{
			PostMotionTarget = DetectionFilter;
			NoMotionTarget = DetectionFilter;  // Also receives non-motion frames for baseline
			LOG_INFO( "Camera %d: ONNX detection enabled (model: %s, confidence: %.2f, max %.1f fps)",
				Camera.ID, Video.DetectionModelPath.c_str(), Video.DetectionConfidence, Video.DetectionMaxFPS );
		}
		else
		{
			LOG_WARNING( "Camera %d: ONNX detection failed to load, falling back to motion-only.", Camera.ID );
		}
	}

	MotionChainNode MVF;
	MVF.OnSuccess = PostMotionTarget;
	MVF.OnFailure = NoMotionTarget;
	MVF.MinimumThreshold = (float)Camera.MDThreshold;
	MVF.InclusiveFilter = ClassificationResult::Motion_Motion;
	MVF.ExclusiveFilter = 0;

	std::shared_ptr<MotionVectorFilter> RootFilter = std::make_shared<MotionVectorFilter>( MVF, Camera.BlackoutMaskPath.c_str(), Camera.FocusMaskPath.c_str() );

	Filter = RootFilter;

	MessageBusObject->SendToClient( nullptr, std::make_shared<CameraStartupMessage>( Camera.ID ) );

	UpdateLastTimedAction("Starting camera connection...");

	CreateInputStream();
}

void CameraWorker::WorkerShutdown()
{
	//Ensure destruction is done on the worker thread
	Filter = nullptr;
	ContinuousStream = nullptr;
	LiveStream = nullptr;
	CameraStream = nullptr;

	MessageBusObject->SendToClient( nullptr, std::make_shared<ThreadShutdownMessage>() );
}

void CameraWorker::WorkerMain()
{
	UpdateLastTimedAction("Work...");

	std::shared_ptr<Message> Msg;
	while( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
		{
			RequestShutdown();
		});

		Msg->Handle<CameraPreviewRequestMessage>([&](const CameraPreviewRequestMessage& Data)
		{
			Observer->SetPreviewTimestamps( Data.LastLargePreviewTimestamp, Data.LastSmallPreviewTimestamp );
		});

		Msg->Handle<CameraStartRecordMessage>([&](const CameraStartRecordMessage& Data)
		{
			OnClipFinished(false);
				
			Observer->SetManualClipStart( Data.Timestamp );
			RecordStream = std::make_shared<OutputStream>( std::string( Data.Path.begin(), Data.Path.end() ), CameraStream.get(), false, false, false, false );
			CameraStreamError InitResult = RecordStream->Initialize();
			if (InitResult != CameraStreamError::Success)
			{
				LOG_ERROR("Recording init failed for camera %d: %s", Camera.ID, RecordStream->GetFFMPEGErrorMessage());
				RecordStream.reset();
			}

			Context->LongPoll->NotifyAll();
		});

		Msg->Handle<CameraStopRecordMessage>([&](const CameraStopRecordMessage& Data)
		{
			OnClipFinished(Data.ManualStop);

			if( Data.ManualStop )
			{
				Filter->ClearState();
			}

			Context->LongPoll->NotifyAll();
		});
	}

	if (IsConnected)
	{
		UpdateLastTimedAction("Processing...");
	}
	else
	{
		UpdateLastTimedAction("Connecting...");
	}

	double FrameRate = CameraStream->GetFramerateDouble();
	double FrameTime = 1.0f / FrameRate;

	const double DeletetionCheckPeriod = 120.0;
	const double BufferPeriodInMilliseconds = 0.0;
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;
	uint64_t Start = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	
	CameraStreamError Error = CameraStream->ProcessFrame( std::static_pointer_cast<IRecordFilter>(Filter), RecordStream.get(), LiveStream.get() );

	if( Error == CameraStreamError::Success )
	{
		if( !IsConnected )
		{
			IsConnected = true;

			MessageBusObject->SendToClient( nullptr, std::make_shared<CameraConnectedMessage>( Camera.ID ) );
		}
	}
	else
	{
		IsConnected = false;

		OnClipFinished(false);

		std::string ErrorStrA = GetCameraStreamErrorMessage(Error);
		if( CameraStream->GetFFMPEGErrorMessage()[0] != '\0')
		{
			ErrorStrA += ": ";
			ErrorStrA += CameraStream->GetFFMPEGErrorMessage();
		}
		std::string ErrorStr(ErrorStrA.begin(), ErrorStrA.end());

		MessageBusObject->SendToClient( nullptr, std::make_shared<CameraReconnectMessage>( Camera.ID, ErrorStr ) );

		CreateInputStream();
		LastFrameTime = 0;

		Camera.JobQueue->RemoveAllForSource( Camera.ID );

		if( Error != CameraStreamError::EndOfFile )
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
	}

	uint64_t End = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	if (!IsRTSP && IsConnected)
	{
		double Duration = (double)(End - Start) / NanoSecondsToSeconds;
		if (Duration < FrameTime)
		{
			double MillisecondsToWait = ((FrameTime - Duration) * 1000.0);

			MillisecondsToWait = std::max( MillisecondsToWait - BufferPeriodInMilliseconds, 0.0 );

			if( MillisecondsToWait > 0.0 )
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(MillisecondsToWait)));
			}
		}
	}

	LastFrameTime = Start;
}

void CameraWorker::OnClipFinished(bool ManualStop)
{
	if (RecordStream)
	{
		Observer->SetManualClipEnd( GetUnixTimestamp() );

		auto FinishedMessage = std::make_shared<CameraClipFinishedMessage>( Camera.ID, ManualStop );
		FinishedMessage->Result = Observer->GetCurrentResult();
		FinishedMessage->ClipStats = Observer->GetClipStatistics();
		MessageBusObject->SendToClient( nullptr, FinishedMessage );
		Filter->ClearState();



		RecordStream->CloseFile();
		RecordStream.reset();
	}
}
