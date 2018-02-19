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
		std::tcerr << U("Unable to open parse config file ") << ConfigFile << U(" due to : ") << Exception.what() << std::endl;
		return false;
		
	}

	if( JsonConfig.has_object_field(U("android")) )
	{
		LoadAndroidSettings( JsonConfig.at(U("android")) );
	}

	if( !JsonConfig.has_object_field(U("server")) )
	{
		std::tcerr << U("Unable to config file. Expect a 'server' section.") << std::endl;
		return false;
	}

	auto JsonServerConfig = JsonConfig.at(U("server"));
	
	if( !CreateListener( JsonServerConfig ) )
	{
		return false;
	}

	if( !InitializeContext() )
	{
		return false;
	}

	Worker = make_unique<AsyncWorker>( Context->MessageBus );
	Worker->Start();

	Watchdog = make_unique<WatchdogWorker>( Context->MessageBus );
	Watchdog->Start();

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

bool WitnessServer::InitializeContext()
{
	auto DatabaseFile = GetConfigFilePath( U("server.db") );

	Context = Server->GetGlobalContext();
	Context->CachePath = CachePath;
	Context->MessageBus = make_shared<MessageBus>();
	Context->Database = Database::InitializeDatabase( DatabaseFile );

	return true;
}

void WitnessServer::StartCameraWorkers()
{
	lock_guard<mutex> Lock( Context->Mutex );

	MAKE_QUERY( GetCameras );

	GetCameras->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			int CameraID = query.GetColumnValueInt( 0 );
			string_t CameraName = query.GetColumnValueText( 1 );
			string_t CameraPath = query.GetColumnValueText( 2 );

			tcout << _T("Starting ") << CameraName << _T(" camera...") << endl;

			auto Worker = make_shared<CameraWorker>( CameraID, CameraPath, Context->MessageBus );
			Worker->Start();
			Watchdog->AddTarget( Worker, CameraName );
			auto& State = Context->Cameras[ CameraID ] = GlobalContext::CameraState();
			State.Worker = Worker;
			State.Name = CameraName;

			return true;
		}
	);
}