#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"


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

int wmain( int argc, wchar_t* argv[] )
{
	auto ConfigFile = GetConfigFilePath( U("server.json") );
	auto DatabaseFile = GetConfigFilePath( U("server.db") );

	std::ifstream ConfigFileStream(ConfigFile);
	if( !ConfigFileStream )
	{
		std::tcerr << U("Unable to open config file ") << ConfigFile << std::endl;
		return 1;
	}
	
	value JsonConfig;
	try
	{
		JsonConfig = value::parse( ConfigFileStream );
	}
	catch( json_exception Exception)
	{
		std::tcerr << U("Unable to open parse config file ") << ConfigFile << U(" due to : ") << Exception.what() << std::endl;
		return 1;
		
	}

	auto JsonConfigAndroid = JsonConfig.at(U("android")).as_object();
	
	string_t ServerKey	= JsonConfigAndroid[U("fcm_server_key")].as_string();
	string_t User		= JsonConfigAndroid[U("fcm_user")].as_string();
	string_t Message	= U("Person at front door");
	string_t Camera		= U("Front Door");
	string_t Image		= JsonConfigAndroid[U("fcm_link")].as_string();

	/*SendAndroidNotification( ServerKey, User, Message, Camera, Image, [](http_response Response){
		wcout << Response.to_string() << endl;
	} );*/

	auto JsonConfigServer = JsonConfig.at(U("server")).as_object();

	string_t Hostname = JsonConfigServer[U("hostname")].as_string();
	int Port = JsonConfigServer[U("port")].as_integer();
	bool Secure = JsonConfigServer[U("secure")].as_bool();


	WitnessListener Listener( Hostname, Port, Secure );

	auto& GC = Listener.GetGlobalContext();
	GC->Database = Database::InitializeDatabase( DatabaseFile );

	Listener.Initialise( JsonConfigServer );
	
	try
	{
		Listener.Start();
	}
	catch( http::http_exception Exception)
	{
		std::tcerr << U("Unable to start server: ") << Exception.what() << std::endl;
		return 1;
	}

	while( true )
	{
		Sleep(100);
	}

	Listener.Stop();

	return 0;
}
