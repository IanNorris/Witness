#include "Common.h"
#include "Witness.h"
#include "CrowListener.h"
#include "SetupServer.h"
#include "AuthHelpers.h"
#include "ClipHelpers.h"
#include "Database.h"


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
			std::cerr << "Setup was not completed. Exiting." << std::endl;
			return false;
		}
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
		std::cout << Errors << std::endl;
		return false;
	}

	//Optional
	Success &= GetSettingsField(Settings, "clip_leadin", Video.ClipHistoryPeriod, Errors);
	Success &= GetSettingsField(Settings, "default_background_algorithm", Video.MotionFilterName, Errors);
	Success &= GetSettingsField(Settings, "export_motion_vectors", Video.ExportMotionVectors, Errors);

	if( !CreateListener( Settings ) )
	{
		return false;
	}

	Context = Server->GetGlobalContext();
	Context->CachePath = CachePath;

	if( !InitializeContext(Database) )
	{
		return false;
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

	std::cout << "Starting web server..." << std::endl;

	Server->Initialise( Settings );

	Timer = std::make_unique<TimerWorker>( Context->MessageBus );
	Timer->Start( WorkerBase::Priority::Normal );

	const int DaysToDelete = 10;

	DeleteOldClips( *Context, DaysToDelete );
	Timer->AddTimer( [&](){
		DeleteOldClips( *Context, DaysToDelete );
	}, 5 * 60 );

	try
	{
		Server->Start();
	}
	catch( std::exception& Exception)
	{
		std::cerr << "Unable to start server: " << Exception.what() << std::endl;
		return 1;
	}

	std::cout << "Starting camera workers..." << std::endl;

	StartCameraWorkers();

	std::cout << "Server boot complete..." << std::endl;

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
		std::cerr << "Hostname is invalid:" << Hostname << std::endl;
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
		std::cout << Errors << std::endl;
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
		std::cout << "Starting " << Camera.Name << " camera..." << std::endl;

		auto Worker = std::make_shared<CameraWorker>(Video, Camera, Context->MessageBus, Context);
		Worker->Start(WorkerBase::Priority::HighPriority);
		Watchdog->AddTarget(Worker, Camera.Name);

		auto& State = Context->GetCameraMap()[Camera.ID] = CameraState();
		State.Worker = Worker;
		State.Name = Camera.Name;
	}
	else
	{
		std::cout << "Skipping " << Camera.Name << " camera, it's disabled..." << std::endl;
	}
}

void WitnessServer::StartCameraWorkers()
{
	std::lock_guard<std::mutex> Lock( Context->Mutex );

	MAKE_QUERY( GetCameras );

	GetCameras->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			StartCamera(query);

			return true;
		}
	);
}
