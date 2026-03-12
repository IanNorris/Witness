#include "Common.h"
#include "Witness.h"
#include "CrowListener.h"
#include "SetupServer.h"
#include "AuthHelpers.h"
#include "ClipHelpers.h"
#include "Database.h"
#include "TagHelpers.h"

#include <Log.h>
#include <ONNXDetectionFilter.h>
#include <FaceDetectionFilter.h>
#include <FaceEmbeddingModel.h>

#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

std::filesystem::path GetConfigFilePath( std::string Filename )
{
#if defined( _WINDOWS )

	size_t RequiredSize = MAX_PATH;
	char ProfileRoot[ MAX_PATH ] = {};
	errno_t Result = getenv_s( &RequiredSize, ProfileRoot, MAX_PATH, "ProgramData" );
	
	assert( Result == S_OK );

	std::filesystem::path ConfigPath = ProfileRoot;
	ConfigPath /= "Witness";

	std::filesystem::create_directories( ConfigPath );
#else
	std::filesystem::path ConfigPath = getenv("HOME");
	ConfigPath /= ".Witness";

	std::filesystem::create_directories( ConfigPath );
#endif

	ConfigPath.append( Filename );

	return ConfigPath;
}

bool WitnessServer::Initialize( DebugConsole* DebugConsoleInstance )
{
	this->DebugConsoleInstance = DebugConsoleInstance;

	auto DatabaseFile = GetConfigFilePath("server.db");

	std::shared_ptr<SQLiteDatabase> Database = Database::InitializeDatabase(DatabaseFile.string());

	// Migrate legacy semicolon-delimited tags to Tag/ClipTag tables
	TagHelpers::MigrateLegacyTags( Database );

	// If no admin user exists, run the web setup wizard first
	if( !Database::HasAdminUser( Database ) )
	{
		// Derive the Web root from the executable directory
		std::filesystem::path exePath;
#ifdef _WIN32
		wchar_t exeBuf[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
		exePath = std::filesystem::path( exeBuf ).parent_path();
#else
		exePath = std::filesystem::canonical( "/proc/self/exe" ).parent_path();
#endif
		std::string staticRoot = ( exePath / "Web" ).string();

		SetupServer setup( Database, staticRoot );
		if( !setup.Run() )
		{
			LOG_ERROR( "Setup was not completed. Exiting." );
			return false;
		}

		LOG_INFO( "Setup complete. Starting production server..." );
	}

	std::unordered_map< std::string, std::string > Settings;

	SQLiteDatabaseQueryInstance GetAllSettings(Database, "GetAllSettings");

	int Result = GetAllSettings->Execute(
		[&](const SQLiteDatabaseQuery& query)
		{
			std::string Name = query.GetColumnValueText(0);
			std::string Value = query.GetColumnValueText(1);
			Settings[Name] = Value;

			return true;
		}
	);

	//Process video settings
	Video.MotionFilterName = "BGS_LBMixtureOfGaussians";
	
	bool Success = true;
	std::string Errors;

	//Required
	//Success &= GetSettingsField(Settings, "data_path", Video.DataPath, Errors);
	//Success &= GetSettingsField(Settings, "face_recognition_cascade_name", Video.FaceCascadeFilter, Errors);
	//Success &= GetSettingsField(Settings, "body_recognition_cascade_name", Video.FullBodyCascadeFilter, Errors);

	if (!Success)
	{
		LOG_ERROR( "%s", Errors.c_str() );
		return false;
	}

	//Optional
	Success &= GetSettingsField(Settings, "clip_leadin", Video.ClipHistoryPeriod, Errors);
	Success &= GetSettingsField(Settings, "default_background_algorithm", Video.MotionFilterName, Errors);
	Success &= GetSettingsField(Settings, "export_motion_vectors", Video.ExportMotionVectors, Errors);

	// Detection settings
	{
		std::string detectionBackend;
		if( GetSettingsField( Settings, "detection_backend", detectionBackend, Errors ) && detectionBackend == "onnx" )
		{
			Video.DetectionEnabled = true;
		}
		GetSettingsField( Settings, "detection_model_path", Video.DetectionModelPath, Errors );
		GetSettingsField( Settings, "detection_confidence", Video.DetectionConfidence, Errors );
		GetSettingsField( Settings, "detection_max_fps", Video.DetectionMaxFPS, Errors );

		std::string detectionProvider;
		if( GetSettingsField( Settings, "detection_provider", detectionProvider, Errors ) && detectionProvider == "gpu" )
		{
			Video.DetectionUseGPU = true;
		}
		GetSettingsField( Settings, "cudnn_path", Video.DetectionCudnnPath, Errors );

		// Default model path if not set
		if( Video.DetectionEnabled && Video.DetectionModelPath.empty() )
		{
#ifdef _WIN32
			wchar_t modelExeBuf[MAX_PATH] = {};
			GetModuleFileNameW( nullptr, modelExeBuf, MAX_PATH );
			auto defaultModel = std::filesystem::path( modelExeBuf ).parent_path() / "models" / "yolo26n.onnx";
#else
			auto defaultModel = std::filesystem::canonical( "/proc/self/exe" ).parent_path() / "models" / "yolo26n.onnx";
#endif
			if( std::filesystem::exists( defaultModel ) )
			{
				Video.DetectionModelPath = defaultModel.string();
			}
			else
			{
				LOG_WARNING( "Detection enabled but no model found at: %s", defaultModel.string().c_str() );
				Video.DetectionEnabled = false;
			}
		}
	}

	// Face detection settings
	{
		std::string faceEnabled;
		if( GetSettingsField( Settings, "face_detection_enabled", faceEnabled, Errors ) && faceEnabled == "1" )
		{
			Video.FaceDetectionEnabled = true;
		}
		LOG_INFO( "Face detection setting: '%s' -> enabled=%s", faceEnabled.c_str(), Video.FaceDetectionEnabled ? "true" : "false" );
		GetSettingsField( Settings, "face_detection_confidence", Video.FaceDetectionConfidence, Errors );
		GetSettingsField( Settings, "face_detection_model_path", Video.FaceDetectionModelPath, Errors );

		// Default face model path if not set
		if( Video.FaceDetectionEnabled && Video.FaceDetectionModelPath.empty() )
		{
#ifdef _WIN32
			wchar_t modelExeBuf[MAX_PATH] = {};
			GetModuleFileNameW( nullptr, modelExeBuf, MAX_PATH );
			auto defaultFaceModel = std::filesystem::path( modelExeBuf ).parent_path() / "models" / "face_detection_yunet_2023mar.onnx";
#else
			auto defaultFaceModel = std::filesystem::canonical( "/proc/self/exe" ).parent_path() / "models" / "face_detection_yunet_2023mar.onnx";
#endif
			if( std::filesystem::exists( defaultFaceModel ) )
			{
				Video.FaceDetectionModelPath = defaultFaceModel.string();
			}
			else
			{
				LOG_WARNING( "Face detection enabled but no model found at: %s", defaultFaceModel.string().c_str() );
				Video.FaceDetectionEnabled = false;
			}
		}
	}

	// Face recognition settings
	{
		std::string faceRecEnabled;
		if( GetSettingsField( Settings, "face_recognition_enabled", faceRecEnabled, Errors ) && faceRecEnabled == "1" )
		{
			Video.FaceRecognitionEnabled = true;
		}
		GetSettingsField( Settings, "face_recognition_confidence", Video.FaceRecognitionConfidence, Errors );
		GetSettingsField( Settings, "face_recognition_model_path", Video.FaceRecognitionModelPath, Errors );
		GetSettingsField( Settings, "face_recognition_min_verified", Video.FaceRecognitionMinVerified, Errors );
		std::string autoAssign;
		if( GetSettingsField( Settings, "face_recognition_auto_assign", autoAssign, Errors ) && autoAssign == "1" )
		{
			Video.FaceRecognitionAutoAssign = true;
		}

		if( Video.FaceRecognitionEnabled && Video.FaceRecognitionModelPath.empty() )
		{
#ifdef _WIN32
			wchar_t modelExeBuf[MAX_PATH] = {};
			GetModuleFileNameW( nullptr, modelExeBuf, MAX_PATH );
			auto defaultRecModel = std::filesystem::path( modelExeBuf ).parent_path() / "models" / "face_recognition.onnx";
#else
			auto defaultRecModel = std::filesystem::canonical( "/proc/self/exe" ).parent_path() / "models" / "face_recognition.onnx";
#endif
			if( std::filesystem::exists( defaultRecModel ) )
			{
				Video.FaceRecognitionModelPath = defaultRecModel.string();
			}
			else
			{
				LOG_WARNING( "Face recognition enabled but no model found at: %s", defaultRecModel.string().c_str() );
				Video.FaceRecognitionEnabled = false;
			}
		}

		LOG_INFO( "Face recognition: %s", Video.FaceRecognitionEnabled ? "enabled" : "disabled" );
	}

	GetSettingsField( Settings, "mse_partial_duration", Video.MsePartialDuration, Errors );

	if( !CreateListener( Settings ) )
	{
		return false;
	}

	Context = Server->GetGlobalContext();
	Context->CachePath = CachePath;
	Context->Events->Start();
	Context->Streams->Start();

	// Initialize face recognition model + cache
	if( Video.FaceRecognitionEnabled )
	{
		auto embModel = std::make_shared<Witness::Camera::FaceEmbeddingModel>();
		if( embModel->LoadModel( Video.FaceRecognitionModelPath.c_str(), Video.DetectionUseGPU, Video.DetectionCudnnPath.c_str() ) )
		{
			Context->FaceEmbeddingModel = embModel;
			Context->FaceCache = std::make_shared<FaceRecognitionCache>();
			// Cache will be loaded after database is initialized
		}
		else
		{
			LOG_WARNING( "Face recognition model failed to load — feature disabled." );
			Video.FaceRecognitionEnabled = false;
		}
	}

	if( !InitializeContext(Database) )
	{
		return false;
	}

	// Load face recognition cache from DB (after DB is ready)
	if( Context->FaceCache && Context->Database )
	{
		Context->FaceCache->SetMinVerifiedCount( Video.FaceRecognitionMinVerified );
		Context->FaceCache->LoadFromDatabase( Context->Database );
	}

	if( !CreateProcessors( Settings ) )
	{
		return false;
	}

	Worker = std::make_unique<AsyncWorker>( Context->MessageBus );
	Worker->Start( WorkerBase::Priority::Normal );

	Watchdog = std::make_unique<WatchdogWorker>( Context->MessageBus );
	Watchdog->Start( WorkerBase::Priority::Normal );

	void* ServerMessageClient = nullptr;
	MessageClient = Context->MessageBus->AddClient( ServerMessageClient );

	LOG_INFO( "Starting web server..." );

	Server->Initialise( Settings );

	Timer = std::make_unique<TimerWorker>( Context->MessageBus );
	Timer->Start( WorkerBase::Priority::Normal );

	const int DaysToDelete = 10;

	// Clip cleanup — disabled by default until verified safe
	std::string clipCleanupEnabled;
	GetSettingsField( Settings, "clip_cleanup_enabled", clipCleanupEnabled, Errors );
	if( clipCleanupEnabled == "true" )
	{
		std::string clipRetentionStr;
		int retentionDays = DaysToDelete;
		if( GetSettingsField( Settings, "clip_retention_days", clipRetentionStr, Errors ) && !clipRetentionStr.empty() )
		{
			int parsed = std::atoi( clipRetentionStr.c_str() );
			if( parsed > 0 ) retentionDays = parsed;
		}

		LOG_INFO( "Clip cleanup enabled: deleting clips older than %d days.", retentionDays );
		DeleteOldClips( *Context, retentionDays );
		Timer->AddTimer( [this, retentionDays](){
			DeleteOldClips( *Context, retentionDays );
		}, 5 * 60 );
	}
	else
	{
		LOG_INFO( "Clip cleanup disabled. Old clips will not be deleted." );
	}

	// Continuous recording cleanup
	{
		std::string contRetentionStr;
		int contRetentionDays = 3; // default
		if( GetSettingsField( Settings, "continuous_recording_retention_days", contRetentionStr, Errors ) && !contRetentionStr.empty() )
		{
			int parsed = std::atoi( contRetentionStr.c_str() );
			if( parsed > 0 ) contRetentionDays = parsed;
		}

		int64_t quotaBytes = 0;
		std::string quotaStr;
		if( GetSettingsField( Settings, "continuous_recording_quota_gb", quotaStr, Errors ) && !quotaStr.empty() )
		{
			int parsed = std::atoi( quotaStr.c_str() );
			if( parsed > 0 ) quotaBytes = static_cast<int64_t>(parsed) * 1024LL * 1024 * 1024;
		}

		// Startup: crash recovery + file size backfill
		CleanupOrphanedContinuousSegments( *Context );
		BackfillContinuousSegmentFileSizes( *Context );

		LOG_INFO( "Continuous recording cleanup: retention %d days, quota %s.",
			contRetentionDays, quotaBytes > 0 ? (quotaStr + " GB").c_str() : "unlimited" );

		DeleteOldContinuousSegments( *Context, contRetentionDays );
		if( quotaBytes > 0 ) EnforceQuotaContinuousSegments( *Context, quotaBytes );
		CheckDiskSpaceSafety( *Context );

		Timer->AddTimer( [this, contRetentionDays, quotaBytes](){
			DeleteOldContinuousSegments( *Context, contRetentionDays );
			if( quotaBytes > 0 ) EnforceQuotaContinuousSegments( *Context, quotaBytes );
			CheckDiskSpaceSafety( *Context );
			CleanupOldDetectionFrames( *Context, contRetentionDays );
		}, 5 * 60 );
	}

	// Build hash broadcast — re-read hash file every 30s, broadcast on change
	Timer->AddTimer( [this](){
		Server->ReadBuildHash();
		auto& ctx = *Context;
		if( !ctx.BuildHash.empty() )
		{
			crow::json::wvalue data;
			data["hash"] = ctx.BuildHash;
			ctx.Events->Broadcast( "build:hash", std::move( data ) );
		}
	}, 30 );

	try
	{
		Server->Start();
	}
	catch( std::exception& Exception)
	{
		LOG_ERROR( "Unable to start server: %s", Exception.what() );
		return 1;
	}

	LOG_INFO( "Starting camera workers..." );

	StartCameraWorkers();

	// Start clip reprocessor if detection is enabled
	if( Video.DetectionEnabled )
	{
		auto reprocessFilter = std::make_shared<Witness::Camera::ONNXDetectionFilter>(
			Witness::Camera::MotionChainNode{},
			Video.DetectionModelPath.c_str(),
			(float)Video.DetectionConfidence,
			Video.DetectionUseGPU,
			0.0f,
			Video.DetectionCudnnPath.empty() ? nullptr : Video.DetectionCudnnPath.c_str()
		);

		if( reprocessFilter->IsModelLoaded() )
		{
			// Create face detection filter for reprocessing if enabled
			std::shared_ptr<Witness::Camera::FaceDetectionFilter> reprocessFaceFilter;
			if( Video.FaceDetectionEnabled && !Video.FaceDetectionModelPath.empty() )
			{
				reprocessFaceFilter = std::make_shared<Witness::Camera::FaceDetectionFilter>(
					Witness::Camera::MotionChainNode{},
					Video.FaceDetectionModelPath.c_str(),
					(float)Video.FaceDetectionConfidence
				);
				if( !reprocessFaceFilter->IsModelLoaded() )
				{
					LOG_WARNING( "Face detection model failed to load for reprocessor" );
					reprocessFaceFilter.reset();
				}
			}

			auto ctx = Context;
			ReprocessWorker = std::make_unique<ClipReprocessWorker>(
				Context->MessageBus,
				Context->Database,
				reprocessFilter,
				reprocessFaceFilter,
				CachePath,
				[ctx]() -> bool
				{
					std::shared_lock<std::shared_mutex> lock( ctx->Mutex );
					for( auto& [id, state] : ctx->GetCameraMap() )
					{
						if( state.IsRecording )
							return false;
					}
					return true;
				}
			);
			ReprocessWorker->Start( WorkerBase::Priority::LowPriority );
			LOG_INFO( "Clip reprocessor started (detection version %d, face detection: %s)", CURRENT_DETECTION_VERSION,
				reprocessFaceFilter ? "enabled" : "disabled" );
		}
	}

	LOG_INFO( "Server boot complete..." );

	return true;
}

bool WitnessServer::CreateListener( const std::unordered_map< std::string, std::string >& Settings )
{
	bool Success = true;
	std::string Errors;

	std::string Hostname;
	int Port;
	std::string Security;
	bool Secure = true;
	std::string CertPath;
	std::string KeyPath;

	Success &= GetSettingsField( Settings, "server_hostname", Hostname, Errors );
	std::vector<std::string> SplitHostname = SplitString(Hostname, ":");

	if (SplitHostname.size() != 2)
	{
		LOG_ERROR( "Hostname is invalid:%s", Hostname.c_str() );
		return false;
	}

	Hostname = SplitHostname[0];
	Port = atoi(SplitHostname[1].c_str());
	
	Success &= GetSettingsField( Settings, "server_tls_mode", Security, Errors );

	if (Security.compare("NoSecurity") == 0)
	{
		Secure = false;
	}

	if( Secure )
	{
		// Cert/key paths are optional in DB — will be validated in CrowListener::Start()
		GetSettingsField( Settings, "server_tls_cert", CertPath, Errors );
		GetSettingsField( Settings, "server_tls_key", KeyPath, Errors );
	}

	Success &= GetSettingsField( Settings, "server_cache", CachePath, Errors );

	if (!Success)
	{
		LOG_ERROR( "%s", Errors.c_str() );
		return false;
	}

	Server = std::make_unique<CrowListener>( Hostname, Port, Secure, CertPath, KeyPath, DebugConsoleInstance );

	return true;
}

bool WitnessServer::CreateProcessors( const std::unordered_map< std::string, std::string >& Settings )
{
	bool Success = true;
	std::string Errors;

	int ThreadCount = 0;

	GetSettingsField( Settings, "thread_count", ThreadCount, Errors );

	if (ThreadCount <= 0)
	{
		ThreadCount = std::thread::hardware_concurrency();
	}

	if (ThreadCount <= 0)
	{
		ThreadCount = 2;
	}
	
	while(ThreadCount--)
	{
		auto ImageWorker = std::make_shared<ImageProcessorWorker>( Context->MessageBus, &CommonImageProcessingJobQueue );
		ImageWorker->Start( WorkerBase::Priority::LowPriority );

		ImageWorkers.push_back(ImageWorker);
	}

	return true;
}

bool WitnessServer::InitializeContext(const std::shared_ptr<SQLiteDatabase>& Database)
{
	Context->MessageBus = std::make_shared<MessageBus>();
	Context->Database = Database;

	Context->CommonImageProcessingJobQueue = &CommonImageProcessingJobQueue;

	return true;
}

void WitnessServer::StartCamera(const SQLiteDatabaseQuery& query)
{
	CameraSettings Camera;

	Camera.ID = query.GetColumnValueInt(0);
	Camera.Name = query.GetColumnValueText(1);
	Camera.Path = query.GetColumnValueText(2);
	Camera.PathSub = query.GetColumnValueText(3);
	Camera.Enabled = query.GetColumnValueInt(5);
	Camera.SkipFrames = query.GetColumnValueInt(6);
	Camera.MDFrameHeight = query.GetColumnValueInt(7);
	Camera.MDThreshold = query.GetColumnValueDouble(8);
	const char* MotionFilterName = query.GetColumnValueText(9);
	const char* BlackoutMaskPath = query.GetColumnValueText(10);
	const char* FocusMaskPath = query.GetColumnValueText(11);

	Camera.BlackoutMaskPath = BlackoutMaskPath ? BlackoutMaskPath : "";
	Camera.FocusMaskPath = FocusMaskPath ? FocusMaskPath : "";

	// ContinuousRecording column added via migration (may be NULL on old DBs)
	Camera.ContinuousRecording = query.GetColumnValueInt(12);
	Camera.LowLatencyHLS = query.GetColumnValueInt(13);

	Camera.MotionFilterName = MotionFilterName && strlen(MotionFilterName) ? MotionFilterName : Video.MotionFilterName.c_str();

	auto FaceCascadeName = Video.FaceCascadeFilter + ".xml";
	auto FaceCascade = (std::filesystem::path(Video.DataPath) / "Cascades" / FaceCascadeName).string();

	auto BodyCascadeName = Video.FullBodyCascadeFilter + ".xml";
	auto BodyCascade = (std::filesystem::path(Video.DataPath) / "Cascades" / BodyCascadeName).string();

	Camera.FaceCascadeFilter = FaceCascade;
	Camera.FullBodyCascadeFilter = BodyCascade;

	Camera.JobQueue = &CommonImageProcessingJobQueue;

	if (Camera.Enabled)
	{
		LOG_INFO( "Starting %s camera...", Camera.Name.c_str() );

		auto Worker = std::make_shared<CameraWorker>(Video, Camera, Context->MessageBus, Context);
		Worker->Start(WorkerBase::Priority::HighPriority);
		Watchdog->AddTarget(Worker, Camera.Name);

		auto& State = Context->GetCameraMap()[Camera.ID] = CameraState();
		State.Worker = Worker;
		State.Name = Camera.Name;
	}
	else
	{
		LOG_INFO( "Skipping %s camera, it's disabled...", Camera.Name.c_str() );
	}
}

void WitnessServer::StartCameraWorkers()
{
	std::unique_lock<std::shared_mutex> Lock( Context->Mutex );

	MAKE_QUERY( GetCameras );

	GetCameras->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			StartCamera(query);

			return true;
		}
	);
}
