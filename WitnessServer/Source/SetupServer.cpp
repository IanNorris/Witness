#include "SetupServer.h"
#include "SetupConfig.h"
#include "AuthHelpers.h"
#include "Common.h"
#include "PlatformHelpers.h"

#include <Log.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <random>

#include "sodium.h"

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
	std::string url = "http://localhost:" + std::to_string( Port );
#ifdef _WIN32
	ShellExecuteA( nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
#else
	// Try common Linux browser openers; ignore failure (headless environments have no browser)
	std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 || "
	                  "open \"" + url + "\" >/dev/null 2>&1 || true";
	std::system( cmd.c_str() );
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

	// Apply setup (create admin user + settings)
	CROW_ROUTE( m_App, "/api/setup/apply" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleApply( req, res );
	});

	// Elevation: launch elevated /apply-config process
	CROW_ROUTE( m_App, "/api/setup/elevate" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleElevate( req, res );
	});

	// Elevation status: poll for completion
	CROW_ROUTE( m_App, "/api/setup/elevate/status" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleElevateStatus( req, res );
	});

	// Settings: return current DB settings for pre-population
	CROW_ROUTE( m_App, "/api/setup/settings" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleSettings( req, res );
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
	data["mode"] = m_HasAdmin ? "reconfigure" : "first_run";
	data["version"] = WITNESS_SETUP_VERSION;

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void SetupServer::HandleSettings( const crow::request& req, crow::response& res )
{
	crow::json::wvalue data;

	// Check if admin user exists to determine mode
	bool hasAdmin = false;
	{
		SQLiteDatabaseQueryInstance query( m_Database, "GetUserCount" );
		query->Execute( [&hasAdmin]( const SQLiteDatabaseQuery& q )
		{
			hasAdmin = q.GetColumnValueInt( 0 ) > 0;
			return true;
		});
	}
	data["mode"] = hasAdmin ? "reconfigure" : "first_run";

	// Load existing settings from DB
	{
		SQLiteDatabaseQueryInstance query( m_Database, "GetAllSettings" );
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

	// Validate required fields (only for first-run, not reconfigure)
	std::string username = body.has( "username" ) ? std::string( body["username"].s() ) : "";
	std::string password = body.has( "password" ) ? std::string( body["password"].s() ) : "";

	if( !m_HasAdmin )
	{
		// First run: username and password are required
		if( username.empty() || password.empty() )
		{
			res.code = 400;
			res.body = R"({"error":"Username and password are required"})";
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
	}
	else
	{
		// Reconfigure: password reset is optional, but validate if provided
		if( !password.empty() )
		{
			if( username.empty() )
			{
				res.code = 400;
				res.body = R"({"error":"Username is required when resetting password"})";
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
		}
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
	if( body.has( "detection_backend" ) )    config.DetectionBackend    = body["detection_backend"].s();
	if( body.has( "detection_provider" ) )   config.DetectionProvider   = body["detection_provider"].s();
	if( body.has( "detection_confidence" ) ) config.DetectionConfidence = body["detection_confidence"].s();
	if( body.has( "detection_max_fps" ) )    config.DetectionMaxFPS     = body["detection_max_fps"].s();
	if( body.has( "cudnn_path" ) )           config.CudnnPath           = body["cudnn_path"].s();

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
	else if( config.TlsMode == "LetsEncrypt" )
	{
		if( !RunLetsEncryptCertbot( config ) )
		{
			res.code = 500;
			res.body = R"({"error":"Let's Encrypt certificate generation failed. Check that certbot is installed and port 80 is available."})";
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

	LOG_INFO( "Setup: Configuration applied successfully." );

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

void SetupServer::HandleElevate( const crow::request& req, crow::response& res )
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

	// Build a config from the request for privileged actions
	SetupConfig config;
	if( body.has( "startup_mode" ) ) config.StartupMode = body["startup_mode"].s();

	// Write pending config to a temp file with random name
	fs::path tempDir = fs::temp_directory_path();
	unsigned char randBytes[8];
	randombytes_buf( randBytes, sizeof( randBytes ) );
	char randHex[17];
	sodium_bin2hex( randHex, sizeof( randHex ), randBytes, sizeof( randBytes ) );
	m_PendingConfigPath = (tempDir / ("witness_setup_" + std::string( randHex ) + ".json")).string();
	std::string statusPath = m_PendingConfigPath + ".status";

	// Remove any stale status file
	fs::remove( statusPath );

	if( !config.SaveToJson( m_PendingConfigPath ) )
	{
		res.code = 500;
		res.body = R"({"error":"Failed to write pending config"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Launch elevated /apply-config process
#ifdef _WIN32
	wchar_t exeBuf[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
	std::wstring exePath = exeBuf;

	std::string params = "/apply-config \"" + m_PendingConfigPath + "\"";
	std::wstring wParams( params.begin(), params.end() );

	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof( sei );
	sei.lpVerb = L"runas";
	sei.lpFile = exePath.c_str();
	sei.lpParameters = wParams.c_str();
	sei.nShow = SW_HIDE;
	sei.fMask = SEE_MASK_NOASYNC;

	if( !ShellExecuteExW( &sei ) )
	{
		DWORD err = GetLastError();
		std::string errMsg = (err == ERROR_CANCELLED)
			? "User cancelled the elevation prompt"
			: "Failed to launch elevated process (error " + std::to_string( err ) + ")";

		res.code = 403;
		crow::json::wvalue errJson;
		errJson["error"] = errMsg;
		res.body = errJson.dump();
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}
#endif

	crow::json::wvalue result;
	result["success"] = true;
	result["message"] = "Elevated process launched. Poll /api/setup/elevate/status for result.";

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void SetupServer::HandleElevateStatus( const crow::request& req, crow::response& res )
{
	if( m_PendingConfigPath.empty() )
	{
		res.code = 404;
		res.body = R"({"status":"no_pending"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	std::string statusPath = m_PendingConfigPath + ".status";

	std::ifstream statusFile( statusPath );
	if( !statusFile )
	{
		// Status file doesn't exist yet — still running
		res.code = 200;
		res.body = R"({"status":"running"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Read and validate status as JSON
	std::string content( (std::istreambuf_iterator<char>(statusFile)),
	                     std::istreambuf_iterator<char>() );
	statusFile.close();

	// Clean up
	fs::remove( statusPath );
	fs::remove( m_PendingConfigPath );
	m_PendingConfigPath.clear();

	// Parse the status file to prevent JSON injection
	auto parsed = crow::json::load( content );
	crow::json::wvalue response;
	response["status"] = "complete";
	if( parsed )
	{
		response["success"] = parsed.has( "success" ) ? parsed["success"].b() : false;
		response["message"] = parsed.has( "message" ) ? std::string( parsed["message"].s() ) : "";
	}
	else
	{
		response["success"] = false;
		response["message"] = "Invalid status from elevated process";
	}

	res.code = 200;
	res.body = response.dump();
	res.set_header( "Content-Type", "application/json" );
	res.end();
}

bool SetupServer::Run()
{
	int port = FindFreePort();

	// Check if admin user already exists (for /websetup reconfigure mode)
	{
		SQLiteDatabaseQueryInstance query( m_Database, "GetUserCount" );
		query->Execute( [this]( const SQLiteDatabaseQuery& q )
		{
			m_HasAdmin = q.GetColumnValueInt( 0 ) > 0;
			return true;
		});
	}

	RegisterRoutes();

	LOG_INFO( "" );
	LOG_INFO( "========================================" );
	LOG_INFO( "  Witness Setup Wizard" );
	LOG_INFO( "========================================" );
	LOG_INFO( "" );
	if( m_HasAdmin )
		LOG_INFO( "Reconfigure your server settings:" );
	else
		LOG_INFO( "No admin account found. Complete setup:" );
	LOG_INFO( "" );
	LOG_INFO( "  http://localhost:%d", port );
	LOG_INFO( "" );
	LOG_INFO( "========================================" );
	LOG_INFO( "" );

	OpenBrowser( port );

	m_App.bindaddr( "127.0.0.1" ).port( port );
	m_App.loglevel( crow::LogLevel::Warning );
	m_App.multithreaded().run();

	return m_SetupComplete;
}

bool SetupServer::GenerateSelfSignedCert( SetupConfig& config )
{
	// Find openssl next to the server executable (Windows puts it there via CMake)
	fs::path exePath = Witness::GetExeDir();

#ifdef _WIN32
	fs::path opensslExe = exePath / "openssl.exe";
#else
	fs::path opensslExe = exePath / "openssl";
#endif
	if( !fs::exists( opensslExe ) )
	{
		// Fall back to system PATH
		opensslExe = "openssl";
	}

	// Extract hostname (strip port) and sanitize for shell safety
	std::string hostname = config.Hostname;
	auto colonPos = hostname.find( ':' );
	if( colonPos != std::string::npos )
		hostname = hostname.substr( 0, colonPos );
	if( hostname.empty() )
		hostname = "localhost";

	// Validate hostname: only allow alphanumeric, dots, hyphens
	for( char c : hostname )
	{
		if( !std::isalnum( static_cast<unsigned char>( c ) ) && c != '.' && c != '-' )
		{
		LOG_ERROR( "Invalid hostname character: %c", c );
			return false;
		}
	}

	// Output paths: ProgramData/Witness on Windows, ~/.Witness on Linux
	fs::path certDir = exePath;
#ifdef _WIN32
	char programData[MAX_PATH] = {};
	size_t reqSize = MAX_PATH;
	if( getenv_s( &reqSize, programData, MAX_PATH, "ProgramData" ) == 0 && reqSize > 0 )
	{
		certDir = fs::path( programData ) / "Witness";
		fs::create_directories( certDir );
	}
#else
	{
		const char* home = getenv( "HOME" );
		if( home )
		{
			certDir = fs::path( home ) / ".Witness";
			fs::create_directories( certDir );
		}
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

	LOG_INFO( "Generating self-signed certificate for %s...", hostname.c_str() );

	// On Windows, cmd /c requires an extra set of outer quotes when the
	// executable path is quoted. Wrap the whole command in quotes.
	std::string fullCmd = envPrefix + cmd;
#ifdef _WIN32
	fullCmd = "\"" + fullCmd + "\"";
#endif

	int result = std::system( fullCmd.c_str() );

	if( result != 0 )
	{
		LOG_ERROR( "OpenSSL certificate generation failed (exit code %d)", result );
		return false;
	}

	config.TlsCertPath = certPath.string();
	config.TlsKeyPath = keyPath.string();

	LOG_INFO( "Certificate generated: %s", certPath.string().c_str() );
	return true;
}

bool SetupServer::RunLetsEncryptCertbot( SetupConfig& config )
{
	// Extract hostname (strip port)
	std::string hostname = config.Hostname;
	auto colonPos = hostname.find( ':' );
	if( colonPos != std::string::npos )
		hostname = hostname.substr( 0, colonPos );
	if( hostname.empty() )
	{
		LOG_ERROR( "Let's Encrypt: hostname is required." );
		return false;
	}

	// Validate hostname
	for( char c : hostname )
	{
		if( !std::isalnum( static_cast<unsigned char>( c ) ) && c != '.' && c != '-' )
		{
			LOG_ERROR( "Let's Encrypt: invalid hostname character: %c", c );
			return false;
		}
	}

	std::string email = config.TlsContact;
	if( email.empty() )
	{
		LOG_ERROR( "Let's Encrypt: contact email is required." );
		return false;
	}

#ifdef _WIN32
	// Check for existing certbot certificate
	fs::path certbotLive = fs::path( "C:\\Certbot\\live" ) / hostname;
	fs::path existingCert = certbotLive / "fullchain.pem";
	fs::path existingKey  = certbotLive / "privkey.pem";

	if( fs::exists( existingCert ) && fs::exists( existingKey ) )
	{
		LOG_INFO( "Let's Encrypt: Using existing certificate for %s", hostname.c_str() );
		config.TlsCertPath = existingCert.string();
		config.TlsKeyPath  = existingKey.string();
		return true;
	}

	// Find certbot executable
	std::string certbotExe;
	const char* searchPaths[] = {
		"C:\\Program Files\\Certbot\\bin\\certbot.exe",
		"C:\\Program Files (x86)\\Certbot\\bin\\certbot.exe",
	};

	for( const char* path : searchPaths )
	{
		if( fs::exists( path ) )
		{
			certbotExe = path;
			break;
		}
	}

	if( certbotExe.empty() )
	{
		// Try PATH
		if( std::system( "where certbot >nul 2>&1" ) == 0 )
			certbotExe = "certbot";
	}

	if( certbotExe.empty() )
	{
		// Try installing via winget (elevated)
		LOG_INFO( "Let's Encrypt: certbot not found, attempting install via winget..." );

		SHELLEXECUTEINFOW sei = {};
		sei.cbSize = sizeof(sei);
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;
		sei.lpVerb = L"runas";
		sei.lpFile = L"winget";
		sei.lpParameters = L"install EFF.Certbot --accept-source-agreements --accept-package-agreements";
		sei.nShow = SW_SHOW;

		if( ShellExecuteExW( &sei ) && sei.hProcess )
		{
			WaitForSingleObject( sei.hProcess, 120000 );
			DWORD exitCode = 1;
			GetExitCodeProcess( sei.hProcess, &exitCode );
			CloseHandle( sei.hProcess );

			if( exitCode == 0 )
			{
				for( const char* path : searchPaths )
				{
					if( fs::exists( path ) )
					{
						certbotExe = path;
						break;
					}
				}
			}
		}

		if( certbotExe.empty() )
		{
			LOG_ERROR( "Let's Encrypt: certbot not found after install attempt. Install manually from https://certbot.eff.org/" );
			return false;
		}
	}

	std::string certbotArgs = "certonly --standalone -d " + hostname +
		" --agree-tos --email " + email + " --non-interactive";

	LOG_INFO( "Let's Encrypt: Running certbot for %s...", hostname.c_str() );

	// certbot needs elevation (binds port 80)
	int wlen = MultiByteToWideChar( CP_UTF8, 0, certbotExe.c_str(), -1, nullptr, 0 );
	std::wstring wideCertbot( wlen, 0 );
	MultiByteToWideChar( CP_UTF8, 0, certbotExe.c_str(), -1, &wideCertbot[0], wlen );

	int alen = MultiByteToWideChar( CP_UTF8, 0, certbotArgs.c_str(), -1, nullptr, 0 );
	std::wstring wideArgs( alen, 0 );
	MultiByteToWideChar( CP_UTF8, 0, certbotArgs.c_str(), -1, &wideArgs[0], alen );

	SHELLEXECUTEINFOW winSei = {};
	winSei.cbSize = sizeof(winSei);
	winSei.fMask = SEE_MASK_NOCLOSEPROCESS;
	winSei.lpVerb = L"runas";
	winSei.lpFile = wideCertbot.c_str();
	winSei.lpParameters = wideArgs.c_str();
	winSei.nShow = SW_SHOW;

	if( !ShellExecuteExW( &winSei ) || !winSei.hProcess )
	{
		LOG_ERROR( "Let's Encrypt: Failed to launch certbot (elevation may have been cancelled)." );
		return false;
	}

	WaitForSingleObject( winSei.hProcess, 120000 );
	DWORD exitCode = 1;
	GetExitCodeProcess( winSei.hProcess, &exitCode );
	CloseHandle( winSei.hProcess );

	if( exitCode != 0 )
	{
		LOG_ERROR( "Let's Encrypt: certbot failed (exit code %lu)", exitCode );
		return false;
	}

	if( !fs::exists( existingCert ) || !fs::exists( existingKey ) )
	{
		LOG_ERROR( "Let's Encrypt: Certificate files not found at %s", certbotLive.string().c_str() );
		return false;
	}

	config.TlsCertPath = existingCert.string();
	config.TlsKeyPath  = existingKey.string();

	LOG_INFO( "Let's Encrypt: Certificate obtained: %s", existingCert.string().c_str() );
	return true;

#else // Linux / POSIX
	// On Linux, certbot is typically installed via the system package manager.
	// We run it directly (elevation via sudo or systemd-run is the user's responsibility).
	std::string certbotExe = "certbot";

	// Check for existing certificate (standard certbot path on Linux)
	fs::path certbotLive  = fs::path( "/etc/letsencrypt/live" ) / hostname;
	fs::path existingCert = certbotLive / "fullchain.pem";
	fs::path existingKey  = certbotLive / "privkey.pem";

	if( fs::exists( existingCert ) && fs::exists( existingKey ) )
	{
		LOG_INFO( "Let's Encrypt: Using existing certificate for %s", hostname.c_str() );
		config.TlsCertPath = existingCert.string();
		config.TlsKeyPath  = existingKey.string();
		return true;
	}

	if( std::system( "command -v certbot >/dev/null 2>&1" ) != 0 )
	{
		LOG_ERROR( "Let's Encrypt: certbot not found. Install it with: sudo apt install certbot" );
		return false;
	}

	std::string cmd = certbotExe +
		" certonly --standalone -d " + hostname +
		" --agree-tos --email " + email +
		" --non-interactive";

	LOG_INFO( "Let's Encrypt: Running: %s", cmd.c_str() );

	int result = std::system( cmd.c_str() );
	if( result != 0 )
	{
		LOG_ERROR( "Let's Encrypt: certbot failed (exit code %d). Run as root or use sudo.", result );
		return false;
	}

	if( !fs::exists( existingCert ) || !fs::exists( existingKey ) )
	{
		LOG_ERROR( "Let's Encrypt: Certificate files not found at %s", certbotLive.string().c_str() );
		return false;
	}

	config.TlsCertPath = existingCert.string();
	config.TlsKeyPath  = existingKey.string();

	LOG_INFO( "Let's Encrypt: Certificate obtained: %s", existingCert.string().c_str() );
	return true;
#endif
}
