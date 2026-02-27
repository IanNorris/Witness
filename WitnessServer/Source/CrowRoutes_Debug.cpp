#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
// ===== Debug Handlers =====

void CrowListener::HandleDebugEnum( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::vector<crow::json::wvalue> Array;

	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		crow::json::wvalue ValueOut;
		ValueOut["name"] = Value->GetName();
		ValueOut["value"] = Value->Get();
		Array.push_back( std::move( ValueOut ) );
	}

	crow::json::wvalue Data;
	Data["values"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugSet( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("name") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string Name = body["name"].s();
	std::string ValueIn = body["value"].s();

	bool Success = false;
	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		if( Name == Value->GetName() )
		{
			Success = Value->Set( ValueIn.c_str() );
			break;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Success ? 200 : 400;
	res.end();
}

void CrowListener::HandleDebugReset( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("name") )
	{
		res.code = 400;
		res.body = "Missing name field";
		res.end();
		return;
	}

	std::string Name = body["name"].s();

	bool Success = false;
	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		if( Name == Value->GetName() )
		{
			Value->Reset();
			Success = true;
			break;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Success ? 200 : 400;
	res.end();
}

void CrowListener::Start()
{
	// Crow/ASIO requires a numeric IP, not a hostname
	std::string bindAddr = m_Hostname;
	if( bindAddr == "localhost" )
		bindAddr = "127.0.0.1";
	else if( bindAddr == "+" || bindAddr == "*" || bindAddr == "0.0.0.0" )
		bindAddr = "0.0.0.0";

	m_App.bindaddr( bindAddr ).port( m_Port );

	m_ServerThread = std::thread( [this]()
	{
		m_App.loglevel( crow::LogLevel::Warning );
		m_App.multithreaded().run();
	});

	printf( "Crow server started on %s:%d\n", m_Hostname.c_str(), m_Port );
}

void CrowListener::Stop()
{
	m_App.stop();

	if( m_ServerThread.joinable() )
	{
		m_ServerThread.join();
	}
}