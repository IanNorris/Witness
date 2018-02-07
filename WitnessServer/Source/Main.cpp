#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "sodium.h"
#include "ObservingMotionFilter.h"

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
	if( sodium_init() == -1 )
	{
		std::tcerr << U("Unable to initialize libsodium.") << std::endl;
        return 1;
    }

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
	string_t MessageStr	= U("Person at front door");
	string_t Camera		= U("Front Door");
	string_t Image		= JsonConfigAndroid[U("fcm_link")].as_string();

	/*SendAndroidNotification( ServerKey, User, MessageStr, Camera, Image, [](http_response Response){
		wcout << Response.to_string() << endl;
	} );*/

	auto JsonConfigServer = JsonConfig.at(U("server")).as_object();

	string_t Hostname = JsonConfigServer[U("hostname")].as_string();
	int Port = JsonConfigServer[U("port")].as_integer();
	bool Secure = JsonConfigServer[U("secure")].as_bool();
	string_t CachePath = JsonConfigServer[U("cache_path")].as_string();

	WitnessListener Listener( Hostname, Port, Secure );

	auto& GC = Listener.GetGlobalContext();
	GC->CachePath = CachePath;
	GC->MessageBus = make_shared<MessageBus>();
	GC->Database = Database::InitializeDatabase( DatabaseFile );

	OfflineCreationForFirstUser( GC );

	tcout << _T("Starting web server...") << endl;

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

	tcout << _T("Starting camera workers...") << endl;

	void* ServerMessageClient = nullptr;
	auto MessageClient = GC->MessageBus->AddClient( ServerMessageClient );

	{
		lock_guard<mutex> Lock( GC->Mutex );

		SQLiteDatabaseQueryInstance GetCameras( GC->Database, _T("GetCameras") );
		GetCameras->Execute( 
			[&GC]( const SQLiteDatabaseQuery& query )
			{
				int CameraID = query.GetColumnValueInt( 0 );
				string_t CameraName = query.GetColumnValueText( 1 );
				string_t CameraPath = query.GetColumnValueText( 2 );

				tcout << _T("Starting ") << CameraName << _T(" camera...") << endl;

				auto Worker = make_shared<CameraWorker>( CameraID, CameraPath, GC->MessageBus );
				auto& State = GC->Cameras[ CameraID ] = GlobalContext::CameraState();
				State.Worker = Worker;
				State.Name = CameraName;

				return true;
			}
		);
	}

	tcout << _T("Server boot complete...") << endl;

	string_t Errors;

	auto StartCameraRecording = [&GC,&CachePath](const shared_ptr<CameraWorker>& Worker, int CameraID, bool IsManual)
	{
		CreateDirectoryW( CachePath.c_str(), nullptr );
		stringstream_t TargetFilename;
		TargetFilename << CachePath << _T("\\") << CameraID << (IsManual ? _T("_Manual_") : _T("_Auto_")) << datetime::utc_timestamp() << ".mp4";

		auto StartRecord = make_shared<CameraStartRecordMessage>( CameraID, TargetFilename.str() );
			
		if( Worker )
		{
			GC->MessageBus->SendToClient( Worker.get(), StartRecord );
		}
	};

	while( true )
	{
		shared_ptr<Message> Msg;
		MessageClient->Pop( Msg );

		auto StatusMessage = [&GC]( int Camera, string_t Reason )
		{
			string_t CameraName;

			{
				lock_guard<mutex> Lock( GC->Mutex );
			
				auto Iter = GC->Cameras.find( Camera );
				if( Iter != GC->Cameras.end() )
				{
					CameraName = (*Iter).second.Name;
				}
			}

			tcout << CameraName << _T(": ") << Reason << endl;
		};

		Msg->Handle<CameraStartupMessage>([&](const CameraStartupMessage& Data)
		{
			StatusMessage( Data.Camera, _T("Online") );
		});

		Msg->Handle<CameraShutdownMessage>([&](const CameraShutdownMessage& Data)
		{
			StatusMessage( Data.Camera, _T("Offline") );
		});

		Msg->Handle<CameraReconnectMessage>([&](const CameraReconnectMessage& Data)
		{
			StatusMessage( Data.Camera, Data.Error );

			{
				lock_guard<mutex> Lock( GC->Mutex );
			
				auto Iter = GC->Cameras.find( Data.Camera );
				if( Iter != GC->Cameras.end() )
				{
					(*Iter).second.IsRecording = false;
					(*Iter).second.IsManualRecording = false;
				}
			}
		});

		Msg->Handle<CameraSnapshotMessage>([&](const CameraSnapshotMessage& Data)
		{
			{
				lock_guard<mutex> Lock( GC->Mutex );
			
				auto Iter = GC->Cameras.find( Data.Camera );
				if( Iter != GC->Cameras.end() )
				{
					(*Iter).second.PreviewThumbnail = Data.Jpeg;
				}
			}
		});

		Msg->Handle<CameraBeginMotionMessage>([&](const CameraBeginMotionMessage& Data)
		{
			shared_ptr<CameraWorker> Worker;
			string_t CameraName;

			{
				lock_guard<mutex> Lock( GC->Mutex );
			
				auto Iter = GC->Cameras.find( Data.Camera );
				if( Iter != GC->Cameras.end() )
				{
					(*Iter).second.ClipThumbnails[ Data.Timestamp ] = Data.Jpeg;
					CameraName = (*Iter).second.Name;
					Worker = (*Iter).second.Worker;

					//Already recording
					if ((*Iter).second.IsRecording)
					{
						return;
					}

					(*Iter).second.IsRecording = true;
				}
			}

			stringstream_t Message;
			Message << _T("Begin Motion: ") << Data.MotionPercentage;

			StatusMessage( Data.Camera, Message.str() );

            stringstream_t ThumbPath;
            ThumbPath << Listener.GetBaseUri() << _T("clip/thumb/") << Data.Camera << _T("/") << Data.Timestamp;
						
			SendAndroidNotification( ServerKey, User, MessageStr, CameraName, ThumbPath.str(), nullptr );
						
			StartCameraRecording( Worker, Data.Camera, false );
		});

		Msg->Handle<CameraUpdateMotionMessage>([&](const CameraUpdateMotionMessage& Data)
		{
			{
				lock_guard<mutex> Lock( GC->Mutex );
			
				auto Iter = GC->Cameras.find( Data.Camera );
				if( Iter != GC->Cameras.end() )
				{
					(*Iter).second.ClipThumbnails[ Data.TimestampStarted ] = Data.Jpeg;
				}
			}
		});

		Msg->Handle<CameraEndMotionMessage>([&](const CameraEndMotionMessage& Data)
		{
			StatusMessage( Data.Camera, _T("End Motion") );

			auto StopRecord = make_shared<CameraStopRecordMessage>( Data.Camera );

			shared_ptr<CameraWorker> Worker;

			{
				lock_guard<mutex> Lock( GC->Mutex );

				auto Iter = GC->Cameras.find( Data.Camera );
				if( Iter != GC->Cameras.end() )
				{
					Worker = (*Iter).second.Worker;

					if ((*Iter).second.IsManualRecording)
					{
						return;
					}

					(*Iter).second.IsRecording = false;
				}
			}

			if( Worker )
			{
				GC->MessageBus->SendToClient( Worker.get(), StopRecord );
			}
		});

		Msg->Handle<CameraStateToggleRecordMessage>([&](const CameraStateToggleRecordMessage& Data)
		{
			StatusMessage( Data.Camera, Data.Record ? _T("Manual Record: On") : _T("Manual Record: Off") );

			shared_ptr<CameraWorker> Worker;

			bool Change = false;

			{
				lock_guard<mutex> Lock( GC->Mutex );

				auto Iter = GC->Cameras.find( Data.Camera );
				if( Iter != GC->Cameras.end() )
				{
					Worker = (*Iter).second.Worker;
					(*Iter).second.IsManualRecording = Data.Record;

					if( (*Iter).second.IsRecording != Data.Record )
					{
						(*Iter).second.IsRecording = Data.Record;
						Change = true;
					}
				}
			}

			if( Change && Worker )
			{
				if( Data.Record )
				{
					StartCameraRecording( Worker, Data.Camera, true );
				}
				else
				{
					auto StopRecord = make_shared<CameraStopRecordMessage>( Data.Camera );
					GC->MessageBus->SendToClient( Worker.get(), StopRecord );
				}
			}
		});
	}

	GC->MessageBus->RemoveClient( ServerMessageClient );

	Listener.Stop();

	return 0;
}
