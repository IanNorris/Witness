#include "Listener.h"
#include <functional>

#include "Commands/Authenticate.h"
#include "Commands/Static.h"
#include "Commands/Camera.h"
#include "Commands/Clip.h"
#include "Commands/Group.h"
#include "Commands/Debug.h"
#include "Commands/Stream.h"

WitnessListener::WitnessListener( utility::string_t Hostname, int Port, bool Secure, DebugConsole* DebugConsoleInstance )
: DebugConsoleInstance( DebugConsoleInstance )
{
	m_GlobalContext = make_unique<GlobalContext>();
	m_GlobalContext->Port = Port;

	web::http::experimental::listener::http_listener_config Config;

	//TODO For Linux:
	/*
	Config.set_ssl_context_callback([](boost::asio::ssl::context& Context)
	{
		//
	});*/

	uri_builder Uri;
	Uri.set_scheme( Secure ? U("https") : U("http") );
	Uri.set_host( Hostname );
	Uri.set_port( Port );

	m_BaseUri = Uri.to_string();

	m_Listener = make_unique<http_listener>( Uri.to_uri(), Config );

	m_Listener->support( methods::GET, std::bind( &WitnessListener::OnCommand, this, std::placeholders::_1, false ) );
	m_Listener->support( methods::POST, std::bind( &WitnessListener::OnCommand, this, std::placeholders::_1, true ) );
}

WitnessListener::~WitnessListener()
{
	Stop();
}

void WitnessListener::Initialise( const std::unordered_map< string_t, string_t >& Settings )
{
	m_Commands[U("auth")] = make_unique<Command_Authenticate>( m_GlobalContext->Port );
	m_Commands[U("static")] = make_unique<Command_Static>( Settings );
	m_Commands[U("camera")] = make_unique<Command_Camera>();
	m_Commands[U("clip")] = make_unique<Command_Clip>();
	m_Commands[U("group")] = make_unique<Command_Group>();
	m_Commands[U("debug")] = make_unique<Command_Debug>( DebugConsoleInstance );
	m_Commands[U("stream")] = make_unique<Command_Stream>();
}

void WitnessListener::Start()
{
	m_Listener->open().then( 
		[]( pplx::task<void> previousTask )
		{
			try
			{
				previousTask.get();
			}
			catch( http_exception e )
			{
				cerr << "Unable to start server: " << e.what() << endl;
				exit(1);
			}
		}
	);
}

void WitnessListener::Stop()
{
	m_Listener->close().wait();
}

void WitnessListener::OnCommand( http_request Message, bool IsPost )
{
	auto Path = http::uri::split_path(http::uri::decode(Message.relative_uri().path()));

	if( !Path.empty() )
	{
		auto CommandName = Path.front();

		auto FoundCommand = m_Commands.find( CommandName );
		if( FoundCommand != m_Commands.end() )
		{
			Path.erase( Path.begin() );

			(*FoundCommand).second->OnMessage( *m_GlobalContext, Message, CommandName, Path, IsPost );
		}
		else
		{
			m_Commands[U("static")]->OnMessage( *m_GlobalContext, Message, U(""), Path, IsPost );
		}
	}
	else
	{
		m_Commands[U("static")]->OnMessage( *m_GlobalContext, Message, U(""), Path, IsPost );
	}
}
