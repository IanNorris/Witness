#include "Common.h"
#include "Witness.h"
#include "Listener.h"
#include "Commands/Authenticate.h"
#include "Database.h"

using namespace web::json;
using namespace web::http::client;
using namespace utility;

#include <windows.h>

std::tr2::sys::path GetConfigFilePath( string_t Filename )
{
#if defined( _WINDOWS )

	size_t RequiredSize = MAX_PATH;
	TCHAR ProfileRoot[ MAX_PATH ] = {};
	errno_t Result = _tgetenv_s( &RequiredSize, ProfileRoot, U("APPDATA") );
	
	assert( Result == S_OK );

	std::tr2::sys::path ConfigPath = ProfileRoot;
	ConfigPath.append( U("Witness") );

	CreateDirectory( ConfigPath.c_str(), NULL );
#else
	std::tr2::sys::path ConfigPath = getenv("HOME");
	ConfigPath.append( U(".Witness") );

	mkdir( ConfigPath.c_str(), 0600 );
#endif

	ConfigPath.append( Filename );

	return ConfigPath;
}

bool WitnessServer::Initialize()
{
	auto ConfigFile = GetConfigFilePath( U("server.json") );

	std::ifstream ConfigFileStream(ConfigFile);
	if( !ConfigFileStream )
	{
		std::tcerr << U("Unable to open config file ") << ConfigFile << std::endl;
		return false;
	}
	
	value JsonConfig;
	try
	{
		JsonConfig = value::parse( ConfigFileStream );
	}
	catch( json_exception Exception)
	{
		std::tcerr << U("Unable to parse config file ") << ConfigFile << U(" due to : ") << Exception.what() << std::endl;
		return false;
		
	}

	if( JsonConfig.has_object_field(U("android")) )
	{
		LoadAndroidSettings( JsonConfig.at(U("android")) );
	}

	if( !JsonConfig.has_object_field(U("server")) )
	{
		std::tcerr << U("Missing section in config file. Expected a 'server' section.") << std::endl;
		return false;
	}

	if( !JsonConfig.has_object_field(U("processing")) )
	{
		std::tcerr << U("Missing section in config file. Expected a 'processing' section.") << std::endl;
		return false;
	}
	
	auto JsonServerConfig = JsonConfig.at(U("server"));
	auto JsonProcessingConfig = JsonConfig.at(U("processing"));

	//Process video settings
	Video.MotionFilterName = _T("BGS_LBMixtureOfGaussians");
	if( JsonConfig.has_object_field(U("video")) )
	{
		auto JsonVideoConfig = JsonConfig.at(U("video"));

		bool Success = true;
		string_t Errors;

		//Required
		Success &= GetJsonField( JsonVideoConfig, _T("data_path"), Video.DataPath, Errors );
		Success &= GetJsonField( JsonVideoConfig, _T("face_recognition_cascade_name"), Video.FaceCascadeFilter, Errors );
		Success &= GetJsonField( JsonVideoConfig, _T("body_recognition_cascade_name"), Video.FullBodyCascadeFilter, Errors );

		if (!Success)
		{
			tcout << Errors << endl;
			return false;
		}

		//Optional
		Success &= GetJsonField( JsonVideoConfig, _T("clip_leadin"), Video.ClipHistoryPeriod, Errors );
		Success &= GetJsonField( JsonVideoConfig, _T("default_background_algorithm"), Video.MotionFilterName, Errors );
	}
	else
	{
		std::tcerr << U("Missing section in config file. Expected a 'video' section.") << std::endl;
		return false;
	}

	if( !CreateListener( JsonServerConfig ) )
	{
		return false;
	}

	Context = Server->GetGlobalContext();
	Context->CachePath = CachePath;

	if( !InitializeContext() )
	{
		return false;
	}

	if( !CreateProcessors( JsonProcessingConfig ) )
	{
		return false;
	}

	Worker = make_unique<AsyncWorker>( Context->MessageBus );
	Worker->Start( WorkerBase::Priority::Normal );

	Watchdog = make_unique<WatchdogWorker>( Context->MessageBus );
	Watchdog->Start( WorkerBase::Priority::Normal );

	void* ServerMessageClient = nullptr;
	MessageClient = Context->MessageBus->AddClient( ServerMessageClient );

	OfflineCreationForFirstUser( *Context );

	tcout << _T("Starting web server...") << endl;

	Server->Initialise( JsonServerConfig.as_object() );

	try
	{
		Server->Start();
	}
	catch( http::http_exception Exception)
	{
		std::tcerr << U("Unable to start server: ") << Exception.what() << std::endl;
		return 1;
	}

	tcout << _T("Starting camera workers...") << endl;

	StartCameraWorkers();

	tcout << _T("Server boot complete...") << endl;

	return true;
}

void WitnessServer::LoadAndroidSettings( const json::value& JsonAndroidSettings )
{
	bool Success = true;
	string_t Errors;

	Success &= GetJsonField( JsonAndroidSettings, _T("fcm_server_key"), Android.ServerKey, Errors );
	Success &= GetJsonField( JsonAndroidSettings, _T("fcm_user"), Android.TempUserId, Errors );
	Android.UseAndroid = Success;
}

bool WitnessServer::CreateListener( const json::value& JsonServerSettings )
{
	bool Success = true;
	string_t Errors;

	string_t Hostname;
	int Port;
	bool Secure;

	Success &= GetJsonField( JsonServerSettings, _T("hostname"), Hostname, Errors );
	Success &= GetJsonField( JsonServerSettings, _T("port"), Port, Errors );
	Success &= GetJsonField( JsonServerSettings, _T("secure"), Secure, Errors );
	Success &= GetJsonField( JsonServerSettings, _T("cache_path"), CachePath, Errors );

	if (!Success)
	{
		tcout << Errors << endl;
		return false;
	}

	Server = make_unique<WitnessListener>( Hostname, Port, Secure );

	return true;
}

bool WitnessServer::CreateProcessors(const json::value& JsonProcessorSettings)
{
	bool Success = true;
	string_t Errors;

	int ThreadCount = 2;

	Success &= GetJsonField( JsonProcessorSettings, _T("thread_count"), ThreadCount, Errors );

	if (!Success)
	{
		tcout << Errors << endl;
		return false;
	}
	
	while(ThreadCount--)
	{
		auto ImageWorker = make_shared<ImageProcessorWorker>( Context->MessageBus, &CommonImageProcessingJobQueue );
		ImageWorker->Start( WorkerBase::Priority::LowPriority );

		ImageWorkers.push_back(ImageWorker);
	}

	return true;
}

bool WitnessServer::InitializeContext()
{
	auto DatabaseFile = GetConfigFilePath( U("server.db") );

	Context->MessageBus = make_shared<MessageBus>();
	Context->Database = Database::InitializeDatabase( DatabaseFile );
	Context->CommonImageProcessingJobQueue = &CommonImageProcessingJobQueue;

	return true;
}

void WitnessServer::StartCameraWorkers()
{
	lock_guard<mutex> Lock( Context->Mutex );

	MAKE_QUERY( GetCameras );

	GetCameras->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{

			CameraSettings Camera;

			Camera.ID = query.GetColumnValueInt( 0 );
			Camera.Name = query.GetColumnValueText( 1 );
			Camera.Path = query.GetColumnValueText( 2 );
			Camera.Enabled = query.GetColumnValueInt( 4 );
			Camera.SkipFrames = query.GetColumnValueInt( 5 );
			Camera.MDFrameHeight = query.GetColumnValueInt( 6 );
			Camera.MDThreshold = query.GetColumnValueDouble( 7 );
			const wchar_t* MotionFilterName = query.GetColumnValueText( 8 );


			Camera.MotionFilterName = MotionFilterName && wcslen(MotionFilterName) ? MotionFilterName : Video.MotionFilterName.c_str();

			auto FaceCascadeName = Video.FaceCascadeFilter + _T(".xml");
			auto FaceCascade = Video.DataPath + _T("\\Cascades\\") + FaceCascadeName;

			auto BodyCascadeName = Video.FullBodyCascadeFilter + _T(".xml");
			auto BodyCascade = Video.DataPath + _T("\\Cascades\\") + BodyCascadeName;

			Camera.FaceCascadeFilter = FaceCascade;
			Camera.FullBodyCascadeFilter = BodyCascade;

			Camera.JobQueue = &CommonImageProcessingJobQueue;

			if( Camera.Enabled )
			{
				tcout << _T("Starting ") << Camera.Name << _T(" camera...") << endl;

				auto Worker = make_shared<CameraWorker>( Video, Camera, Context->MessageBus );
				Worker->Start( WorkerBase::Priority::HighPriority );
				Watchdog->AddTarget( Worker, Camera.Name );
				auto& State = Context->Cameras[ Camera.ID ] = CameraState();
				State.Worker = Worker;
				State.Name = Camera.Name;
			}
			else
			{
				tcout << _T("Skipping ") << Camera.Name << _T(" camera, it's disabled...") << endl;
			}

			return true;
		}
	);
}