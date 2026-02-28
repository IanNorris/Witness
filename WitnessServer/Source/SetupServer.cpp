#include "SetupServer.h"
#include "SetupConfig.h"
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

	// Build config from request
	SetupConfig config;
	config.Username = username;
	config.Password = password;

	if( body.has( "hostname" ) )    config.Hostname    = body["hostname"].s();
	if( body.has( "cache_path" ) )  config.CachePath   = body["cache_path"].s();
	if( body.has( "tls_mode" ) )    config.TlsMode     = body["tls_mode"].s();
	if( body.has( "tls_cert" ) )    config.TlsCertPath = body["tls_cert"].s();
	if( body.has( "tls_key" ) )     config.TlsKeyPath  = body["tls_key"].s();
	if( body.has( "tls_contact" ) ) config.TlsContact  = body["tls_contact"].s();

	// Set web root to current static root
	config.WebRoot = m_StaticRoot;

	// Generate self-signed cert if requested
	if( config.TlsMode == "SelfSigned" )
	{
		if( !GenerateSelfSignedCert( config ) )
		{
			res.code = 500;
			res.body = R"({"error":"Failed to generate self-signed certificate"})";
			res.set_header( "Content-Type", "application/json" );
			res.end();
			return;
		}
	}

	// Apply to database
	if( !config.ApplyToDatabase( m_Database ) )
	{
		res.code = 500;
		res.body = R"({"error":"Failed to apply settings"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	std::cout << "Setup: Configuration applied successfully." << std::endl;

	m_SetupComplete = true;

	crow::json::wvalue result;
	result["success"] = true;
	result["message"] = "Setup complete. The server will now restart in production mode.";

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

bool SetupServer::GenerateSelfSignedCert( SetupConfig& config )
{
	// Find openssl.exe next to the server executable
	fs::path exePath;
#ifdef _WIN32
	wchar_t exeBuf[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
	exePath = fs::path( exeBuf ).parent_path();
#else
	exePath = fs::canonical( "/proc/self/exe" ).parent_path();
#endif

	fs::path opensslExe = exePath / "openssl.exe";
	if( !fs::exists( opensslExe ) )
	{
		// Try system PATH
		opensslExe = "openssl";
	}

	// Extract hostname (strip port)
	std::string hostname = config.Hostname;
	auto colonPos = hostname.find( ':' );
	if( colonPos != std::string::npos )
		hostname = hostname.substr( 0, colonPos );
	if( hostname.empty() )
		hostname = "localhost";

	// Output paths in ProgramData/Witness
	fs::path certDir = exePath;
#ifdef _WIN32
	char programData[MAX_PATH] = {};
	size_t reqSize = MAX_PATH;
	if( getenv_s( &reqSize, programData, MAX_PATH, "ProgramData" ) == 0 && reqSize > 0 )
	{
		certDir = fs::path( programData ) / "Witness";
		fs::create_directories( certDir );
	}
#endif

	fs::path certPath = certDir / "selfsigned.pem";
	fs::path keyPath = certDir / "selfsigned-key.pem";

	// Build openssl command
	std::string cmd = "\"" + opensslExe.string() + "\" req -x509 -newkey rsa:2048 -nodes"
		" -days 3650"
		" -keyout \"" + keyPath.string() + "\""
		" -out \"" + certPath.string() + "\""
		" -subj \"/CN=" + hostname + "\"";

	// Set OPENSSL_CONF if config file exists next to openssl
	fs::path opensslCnf = opensslExe.parent_path() / "openssl.cnf";
	std::string envPrefix;
	if( fs::exists( opensslCnf ) )
	{
#ifdef _WIN32
		_putenv_s( "OPENSSL_CONF", opensslCnf.string().c_str() );
#else
		envPrefix = "OPENSSL_CONF=\"" + opensslCnf.string() + "\" ";
#endif
	}

	std::cout << "Generating self-signed certificate for " << hostname << "..." << std::endl;

	int result = std::system( (envPrefix + cmd).c_str() );

	if( result != 0 )
	{
		std::cerr << "OpenSSL certificate generation failed (exit code " << result << ")" << std::endl;
		return false;
	}

	config.TlsCertPath = certPath.string();
	config.TlsKeyPath = keyPath.string();

	std::cout << "Certificate generated: " << certPath.string() << std::endl;
	return true;
}
