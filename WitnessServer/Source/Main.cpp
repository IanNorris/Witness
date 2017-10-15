#include "Listener.h"

#include "Android/AndroidNotify.h"

#include <iostream>
#include <filesystem>
#include <tchar.h>
#include <assert.h>

#if !defined(_WINDOWS)
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(UNICODE) || defined(_UNICODE)
#define tcout wcout
#define tcerr wcerr
#else
#define tcout cout
#define tcerr cerr
#endif

using namespace web::json;
using namespace web::http::client;

#include <windows.h>

std::tr2::sys::path GetConfigFilePath()
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

	ConfigPath.append( U("server.json") );

	return ConfigPath;
}

int wmain( int argc, wchar_t* argv[] )
{
	auto ConfigFile = GetConfigFilePath();

	std::ifstream ConfigFileStream(ConfigFile);
	if( !ConfigFileStream )
	{
		std::tcerr << U("Unable to open config file ") << ConfigFile << std::endl;
		return 1;
	}
	
	json::value JsonConfig;
	try
	{
		JsonConfig = json::value::parse( ConfigFileStream );
	}
	catch( web::json::json_exception Exception)
	{
		std::tcerr << U("Unable to open parse config file ") << ConfigFile << U(" due to : ") << Exception.what() << std::endl;
		return 1;
		
	}

	auto JsonConfigAndroid = JsonConfig.at(U("android")).as_object();
	
	utility::string_t ServerKey = JsonConfigAndroid[U("fcm_server_key")].as_string();
	utility::string_t User		= JsonConfigAndroid[U("fcm_user")].as_string();
	utility::string_t Message	= U("Person at front door");
	utility::string_t Camera	= U("Front Door");
	utility::string_t Image		= JsonConfigAndroid[U("fcm_link")].as_string();

	SendAndroidNotification( ServerKey, User, Message, Camera, Image, [](http_response Response){
		wcout << Response.to_string() << endl;
	} );

	auto JsonConfigServer = JsonConfig.at(U("server")).as_object();

	utility::string_t Hostname = JsonConfigServer[U("hostname")].as_string();
	int Port = JsonConfigServer[U("port")].as_integer();


	WitnessListener Listener( Hostname, Port );
	
	try
	{
		Listener.Start();
	}
	catch( web::http::http_exception Exception)
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
