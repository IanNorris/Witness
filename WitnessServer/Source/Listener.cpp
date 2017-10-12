#include "Listener.h"
#include <functional>

#include "Commands/Authenticate.h"

WitnessListener::WitnessListener( utility::string_t Address )
{
	m_GlobalContext = make_unique<GlobalContext>();

	experimental::listener::http_listener_config Config;

	//TODO For Linux:
	/*
	Config.set_ssl_context_callback([](boost::asio::ssl::context& Context)
	{
		//
	});*/

	m_Listener = make_unique<http_listener>( Address, Config );

	m_Listener->support( methods::GET, std::bind( &WitnessListener::OnCommand, this, std::placeholders::_1, false ) );
	m_Listener->support( methods::POST, std::bind( &WitnessListener::OnCommand, this, std::placeholders::_1, true ) );
}

WitnessListener::~WitnessListener()
{
	Stop();
}

void WitnessListener::Start()
{
	m_Commands[U("auth")] = make_unique<Command_Authenticate>();

	m_Listener->open().wait();
}

void WitnessListener::Stop()
{
	m_Listener->close().wait();
}

void WitnessListener::OnCommand( http_request Message, bool IsPost )
{
	auto Path = http::uri::split_path(http::uri::decode(Message.relative_uri().path()));

	if( Path.empty() )
	{
		json::value Reply = json::value::object();
		Reply[U("Version")] = json::value::string(U(WITNESS_LISTENER_VERSION));

		Message.reply(status_codes::OK, Reply);
	}
	else
	{
		auto CommandName = Path.front();
		Path.erase( Path.begin() );		

		auto FoundCommand = m_Commands.find( CommandName );
		if( FoundCommand != m_Commands.end() )
		{
			(*FoundCommand).second->OnMessage( m_GlobalContext, Message, CommandName, Path, IsPost );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}
	}
}
