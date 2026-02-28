#include "SetupServer.h"
#include "AuthHelpers.h"
#include "Common.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <random>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

SetupServer::SetupServer( std::shared_ptr<SQLiteDatabase> Database, const std::string& StaticRoot )
	: m_Database( std::move( Database ) )
	, m_StaticRoot( StaticRoot )
{
}

SetupServer::~SetupServer()
{
}

int SetupServer::FindFreePort()
{
	// Try random ports in the ephemeral range (49152–65535)
	std::random_device rd;
	std::mt19937 gen( rd() );
	std::uniform_int_distribution<int> dist( 49152, 65000 );

	for( int attempts = 0; attempts < 50; ++attempts )
	{
		int port = dist( gen );

		// Try to bind a temporary socket to test availability
		try
		{
			asio::io_context ioc;
			asio::ip::tcp::acceptor acceptor( ioc );
			asio::ip::tcp::endpoint ep( asio::ip::make_address( "127.0.0.1" ), static_cast<unsigned short>( port ) );
			acceptor.open( ep.protocol() );
			acceptor.set_option( asio::ip::tcp::acceptor::reuse_address( true ) );
			acceptor.bind( ep );
			acceptor.close();
			return port;
		}
		catch( ... )
		{
			// Port in use, try another
		}
	}

	return 49200; // fallback
}

void SetupServer::OpenBrowser( int Port )
{
#ifdef _WIN32
	std::string url = "http://localhost:" + std::to_string( Port );
	ShellExecuteA( nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
#endif
}

void SetupServer::RegisterRoutes()
{
	// Setup status API
	CROW_ROUTE( m_App, "/api/setup/status" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleStatus( req, res );
	});

	// Apply setup (create admin user)
	CROW_ROUTE( m_App, "/api/setup/apply" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleApply( req, res );
	});

	// Serve setup static files
	CROW_ROUTE( m_App, "/" )
	([this]( const crow::request& req, crow::response& res )
	{
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
			res.code = 500;
			res.body = "Setup page not found. Ensure Web/setup/index.html exists.";
		}
		res.end();
	});

	CROW_ROUTE( m_App, "/<path>" )
	([this]( const crow::request& req, crow::response& res, const std::string& path )
	{
		// Only serve files from Web/setup/
		fs::path fullPath = fs::path( m_StaticRoot ) / "setup" / path;

		// Prevent path traversal
		auto canonical = fs::weakly_canonical( fullPath );
		auto root = fs::weakly_canonical( fs::path( m_StaticRoot ) / "setup" );
		if( canonical.string().find( root.string() ) != 0 )
		{
			res.code = 403;
			res.end();
			return;
		}

		std::ifstream file( fullPath, std::ios::binary );
		if( file )
		{
			std::string body( (std::istreambuf_iterator<char>(file)),
			                  std::istreambuf_iterator<char>() );

			// Basic MIME type detection
			std::string contentType = "application/octet-stream";
			if( fullPath.extension() == ".html" ) contentType = "text/html";
			else if( fullPath.extension() == ".css" ) contentType = "text/css";
			else if( fullPath.extension() == ".js" ) contentType = "application/javascript";
			else if( fullPath.extension() == ".svg" ) contentType = "image/svg+xml";
			else if( fullPath.extension() == ".png" ) contentType = "image/png";

			res.set_header( "Content-Type", contentType );
			res.body = std::move( body );
			res.code = 200;
		}
		else
		{
			res.code = 404;
		}
		res.end();
	});
}

void SetupServer::HandleStatus( const crow::request& req, crow::response& res )
{
	crow::json::wvalue data;
	data["mode"] = "first_run";
	data["version"] = WITNESS_SETUP_VERSION;

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void SetupServer::HandleApply( const crow::request& req, crow::response& res )
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

	// Validate required fields
	if( !body.has( "username" ) || !body.has( "password" ) )
	{
		res.code = 400;
		res.body = R"({"error":"Username and password are required"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	std::string username = body["username"].s();
	std::string password = body["password"].s();

	if( username.empty() || password.empty() )
	{
		res.code = 400;
		res.body = R"({"error":"Username and password cannot be empty"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	if( password.size() < 8 )
	{
		res.code = 400;
		res.body = R"({"error":"Password must be at least 8 characters"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Normalize username to lowercase
	std::string usernameLC = username;
	std::transform( usernameLC.begin(), usernameLC.end(), usernameLC.begin(), ::tolower );

	// Check no users exist yet (prevent race)
	bool hasUsers = true;
	{
		SQLiteDatabaseQueryInstance query( m_Database, "GetUserCount" );
		query->Execute( [&hasUsers]( const SQLiteDatabaseQuery& q )
		{
			hasUsers = q.GetColumnValueInt( 0 ) > 0;
			return true;
		});
	}

	if( hasUsers )
	{
		res.code = 409;
		res.body = R"({"error":"An admin user already exists"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Hash password and create admin user
	std::string hash = GetHashedPasswordKey_Algorithm0( usernameLC, password );

	{
		SQLiteDatabaseQueryInstance createUser( m_Database, "CreateUser" );
		createUser->Bind( "@Username", usernameLC.c_str() );
		createUser->Bind( "@DisplayName", username.c_str() );
		createUser->Bind( "@PasswordHash", hash.c_str() );
		createUser->Bind( "@HashMethod", 0 );
		createUser->Bind( "@Enabled", 1 );
		createUser->Bind( "@Admin", 1 );
		createUser->Execute( nullptr );
	}

	std::cout << "Setup: Admin user '" << username << "' created successfully." << std::endl;

	m_SetupComplete = true;

	crow::json::wvalue result;
	result["success"] = true;
	result["message"] = "Admin account created. The server will now restart in production mode.";

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();

	// Stop the setup server after a brief delay to let the response send
	std::thread( [this]()
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
		m_App.stop();
	}).detach();
}

bool SetupServer::Run()
{
	int port = FindFreePort();

	RegisterRoutes();

	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << "  Witness Setup Wizard" << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << std::endl;
	std::cout << "No admin account found." << std::endl;
	std::cout << "Open your browser to complete setup:" << std::endl;
	std::cout << std::endl;
	std::cout << "  http://localhost:" << port << std::endl;
	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << std::endl;

	OpenBrowser( port );

	m_App.bindaddr( "127.0.0.1" ).port( port );
	m_App.loglevel( crow::LogLevel::Warning );
	m_App.multithreaded().run();

	return m_SetupComplete;
}
