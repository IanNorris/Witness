#include "Common.h"
#include "Witness.h"
#include "Listener.h"
#include "Commands/Authenticate.h"
#include "Commands/Clip.h"
#include "Database.h"

using namespace web::json;
using namespace web::http::client;
using namespace utility;

#include <filesystem>
#include <windows.h>

std::experimental::filesystem::path GetConfigFilePath( string_t Filename )
{
#if defined( _WINDOWS )

	size_t RequiredSize = MAX_PATH;
	TCHAR ProfileRoot[ MAX_PATH ] = {};
	errno_t Result = _tgetenv_s( &RequiredSize, ProfileRoot, U("ProgramData") );
	
	assert( Result == S_OK );

	std::experimental::filesystem::path ConfigPath = ProfileRoot;
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

bool WitnessServer::Initialize( DebugConsole* DebugConsoleInstance )
{
	this->DebugConsoleInstance = DebugConsoleInstance;

	auto DatabaseFile = GetConfigFilePath(U("server.db"));

	shared_ptr<SQLiteDatabase> Database = Database::InitializeDatabase(DatabaseFile);

	std::unordered_map< string_t, string_t > Settings;

	SQLiteDatabaseQueryInstance GetAllSettings(Database, _T("GetAllSettings"));

	int Result = GetAllSettings->Execute(
		[&](const SQLiteDatabaseQuery& query)
		{
			string_t Name = query.GetColumnValueText(0);
			string_t Value = query.GetColumnValueText(1);
			Settings[Name] = Value;

			return true;
		}
	);

	LoadAndroidSettings(Settings);

	//Process video settings
	Video.MotionFilterName = _T("BGS_LBMixtureOfGaussians");
	
	bool Success = true;
	string_t Errors;

	//Required
	//Success &= GetSettingsField(Settings, _T("data_path"), Video.DataPath, Errors);
	//Success &= GetSettingsField(Settings, _T("face_recognition_cascade_name"), Video.FaceCascadeFilter, Errors);
	//Success &= GetSettingsField(Settings, _T("body_recognition_cascade_name"), Video.FullBodyCascadeFilter, Errors);

	if (!Success)
	{
		tcout << Errors << endl;
		return false;
	}

	//Optional
	Success &= GetSettingsField(Settings, _T("clip_leadin"), Video.ClipHistoryPeriod, Errors);
	Success &= GetSettingsField(Settings, _T("default_background_algorithm"), Video.MotionFilterName, Errors);
	Success &= GetSettingsField(Settings, _T("export_motion_vectors"), Video.ExportMotionVectors, Errors);

	if( !CreateListener( Settings ) )
	{
		return false;
	}

	Context = Server->GetGlobalContext();
	Context->CachePath = CachePath;

	/*if( JsonConfig.has_object_field(L"azure") )
	{
		auto AzureRoot = JsonConfig.at(L"azure").as_object();
		for( auto Iter = AzureRoot.cbegin(); Iter != AzureRoot.cend(); ++Iter )
		{
			if( (*Iter).second.is_object() )
			{
				Context->AzureSettings.push_back(SettingsMap());
				auto& Settings = Context->AzureSettings.back();

				Settings.Name = (*Iter).first;
				auto Child = (*Iter).second.as_object();

				for( auto ChildIter = Child.cbegin(); ChildIter != Child.cend(); ++ChildIter )
				{
					if( (*ChildIter).second.is_string() )
					{
						auto& ChildValue = (*ChildIter).second.as_string();

						auto& KVP = Settings.Settings[ (*ChildIter).first ] = ChildValue;
					}
				}
			}
		}
	}*/


	if( !InitializeContext(Database) )
	{
		return false;
	}

	if( !CreateProcessors( Settings ) )
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

	Server->Initialise( Settings );

	Timer = make_unique<TimerWorker>( Context->MessageBus );
	Timer->Start( WorkerBase::Priority::Normal );

	const int DaysToDelete = 10;

	Command_Clip::DeleteOldClips( *Context, DaysToDelete );
	Timer->AddTimer( [&](){
		Command_Clip::DeleteOldClips( *Context, DaysToDelete );
	}, 5 * 60 );

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

void WitnessServer::LoadAndroidSettings( const std::unordered_map< string_t, string_t >& Settings )
{
	bool Success = true;
	string_t Errors;

	Success &= GetSettingsField( Settings, _T("fcm_server_key"), Android.ServerKey, Errors );
	Success &= GetSettingsField( Settings, _T("fcm_user"), Android.TempUserId, Errors );
	Android.UseAndroid = Success;
}

bool WitnessServer::CreateListener( const std::unordered_map< string_t, string_t >& Settings )
{
	bool Success = true;
	string_t Errors;

	string_t Hostname;
	int Port;
	string_t Security;
	bool Secure = true;

	Success &= GetSettingsField( Settings, _T("server_hostname"), Hostname, Errors );
	std::vector<string_t> SplitHostname = SplitString(Hostname, _T(":"));

	Hostname = SplitHostname[0];
	Port = atoi(string(SplitHostname[1].begin(), SplitHostname[1].end()).c_str());
	
	Success &= GetSettingsField( Settings, _T("server_tls_mode"), Security, Errors );

	if (Security.compare(_T("NoSecurity")) == 0)
	{
		Secure = false;
	}

	Success &= GetSettingsField( Settings, _T("server_cache"), CachePath, Errors );

	if (!Success)
	{
		tcout << Errors << endl;
		return false;
	}

	Server = make_unique<WitnessListener>( Hostname, Port, Secure, DebugConsoleInstance );

	return true;
}

bool WitnessServer::CreateProcessors( const std::unordered_map< string_t, string_t >& Settings )
{
	bool Success = true;
	string_t Errors;

	int ThreadCount = 0;

	GetSettingsField( Settings, _T("thread_count"), ThreadCount, Errors );

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
		auto ImageWorker = make_shared<ImageProcessorWorker>( Context->MessageBus, &CommonImageProcessingJobQueue );
		ImageWorker->Start( WorkerBase::Priority::LowPriority );

		ImageWorkers.push_back(ImageWorker);
	}

	return true;
}

bool WitnessServer::InitializeContext(const shared_ptr<SQLiteDatabase>& Database)
{
	Context->MessageBus = make_shared<MessageBus>();
	Context->Database = Database;

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
			const wchar_t* BlackoutMaskPath = query.GetColumnValueText( 9 );
			const wchar_t* FocusMaskPath = query.GetColumnValueText( 10 );

			Camera.BlackoutMaskPath = BlackoutMaskPath ? BlackoutMaskPath : L"";
			Camera.FocusMaskPath = FocusMaskPath ? FocusMaskPath : L"";

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

				auto Worker = make_shared<CameraWorker>( Video, Camera, Context->MessageBus, Context );
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