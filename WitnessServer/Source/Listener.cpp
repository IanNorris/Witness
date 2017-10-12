#include "Listener.h"
#include <functional>

WitnessListener::WitnessListener( utility::string_t Address )
{
	experimental::listener::http_listener_config Config;

	//TODO For Linux:
	/*
	Config.set_ssl_context_callback([](boost::asio::ssl::context& Context)
	{
		//
	});*/

	m_Listener = make_unique<http_listener>( Address, Config );

	m_Listener->support( methods::GET, std::bind( &WitnessListener::OnGET, this, std::placeholders::_1 ) );
	m_Listener->support( methods::POST, std::bind( &WitnessListener::OnPOST, this, std::placeholders::_1 ) );
}

WitnessListener::~WitnessListener()
{
	Stop();
}

void WitnessListener::Start()
{
	m_Listener->open().wait();
}

void WitnessListener::Stop()
{
	m_Listener->close().wait();
}

void WitnessListener::OnGET( http_request Message )
{
}

void WitnessListener::OnPOST( http_request Message )
{
}
