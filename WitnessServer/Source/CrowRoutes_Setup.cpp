#include "CrowListener.h"
#include "CrowAuth.h"
#include "SetupConfig.h"
#include "GlobalContext.h"

#include <Log.h>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

void CrowListener::HandleSetupPage( const crow::request& req, crow::response& res )
{
	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.body = "Admin authentication required. <a href=\"/\">Login first</a>";
		res.set_header( "Content-Type", "text/html" );
		res.end();
		return;
	}

	// Serve the setup wizard page from Web/setup/
	fs::path indexPath = fs::path( m_StaticRoot ) / "setup" / "index.html";
	std::ifstream file( indexPath, std::ios::binary );
	if( file )
	{
		std::string body( (std::istreambuf_iterator<char>(file)),
		                  std::istreambuf_iterator<char>() );
		res.set_header( "Content-Type", "text/html" );
		res.body = std::move( body );
		res.code = 200;
	}
	else
	{
		res.code = 404;
		res.body = "Setup page not found.";
	}
	res.end();
}

void CrowListener::HandleSetupSettings( const crow::request& req, crow::response& res )
{
	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Read all current settings from DB
	crow::json::wvalue data;
	data["mode"] = "reconfigure";

	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "GetAllSettings" );
		query->Execute( [&data]( const SQLiteDatabaseQuery& q )
		{
			const char* name = q.GetColumnValueText( 0 );
			const char* value = q.GetColumnValueText( 1 );
			if( name && value )
			{
				data[name] = std::string( value );
			}
			return true;
		});
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleSetupApply( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.body = R"({"error":"Invalid JSON"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Build config (no username/password for reconfiguration)
	SetupConfig config;
	if( body.has( "hostname" ) )    config.Hostname    = body["hostname"].s();
	if( body.has( "cache_path" ) )  config.CachePath   = body["cache_path"].s();
	if( body.has( "tls_mode" ) )    config.TlsMode     = body["tls_mode"].s();
	if( body.has( "tls_cert" ) )    config.TlsCertPath = body["tls_cert"].s();
	if( body.has( "tls_key" ) )     config.TlsKeyPath  = body["tls_key"].s();
	if( body.has( "tls_contact" ) ) config.TlsContact  = body["tls_contact"].s();

	// Apply settings to database
	if( !config.ApplyToDatabase( m_GlobalContext->Database ) )
	{
		res.code = 500;
		res.body = R"({"error":"Failed to apply settings"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	LOG_INFO( "Settings updated via /setup by admin (UserUID=%d)", UserUID );

	// Reload TLS if cert paths changed
	if( !config.TlsCertPath.empty() || !config.TlsKeyPath.empty() )
	{
		if( !config.TlsCertPath.empty() ) m_CertPath = config.TlsCertPath;
		if( !config.TlsKeyPath.empty() )  m_KeyPath = config.TlsKeyPath;
		ReloadTLS();
	}

	crow::json::wvalue result;
	result["success"] = true;
	result["message"] = "Settings applied. Some changes may require a server restart.";

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}
