#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"

#include <filesystem>
#include <iostream>
#include <chrono>

#ifdef CROW_ENABLE_SSL
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#endif
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

void CrowListener::HandleDebugReloadTLS( const crow::request& req, crow::response& res )
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

	bool Success = ReloadTLS();

	crow::json::wvalue Data;
	Data["success"] = Success;
	Data["message"] = Success ? "TLS certificate reloaded" : "TLS reload failed — check server logs";

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = Success ? 200 : 500;
	res.end();
}

#ifdef CROW_ENABLE_SSL
static bool LogCertExpiry( const std::string& certPath )
{
	FILE* fp = fopen( certPath.c_str(), "r" );
	if( !fp )
	{
		std::cerr << "TLS: Unable to open certificate file: " << certPath << std::endl;
		return false;
	}

	X509* cert = PEM_read_X509( fp, nullptr, nullptr, nullptr );
	fclose( fp );

	if( !cert )
	{
		std::cerr << "TLS: Unable to parse certificate: " << certPath << std::endl;
		return false;
	}

	const ASN1_TIME* notAfter = X509_get0_notAfter( cert );
	int pday = 0, psec = 0;
	ASN1_TIME_diff( &pday, &psec, nullptr, notAfter );

	bool expired = ( pday < 0 || ( pday == 0 && psec < 0 ) );

	if( expired )
	{
		std::cerr << "TLS WARNING: Certificate has expired! (" << certPath << ")" << std::endl;
		std::cerr << "TLS WARNING: Server will start but clients may reject the connection." << std::endl;
	}
	else
	{
		std::cout << "TLS: Certificate expires in " << pday << " days (" << certPath << ")" << std::endl;
		if( pday < 30 )
		{
			std::cerr << "TLS WARNING: Certificate expires in less than 30 days — consider renewing." << std::endl;
		}
	}

	X509_free( cert );
	return true;
}
#endif

bool CrowListener::ConfigureSSL()
{
#ifdef CROW_ENABLE_SSL
	if( !m_Secure )
		return true;

	namespace fs = std::filesystem;

	if( m_CertPath.empty() || m_KeyPath.empty() )
	{
		std::cerr << "TLS ERROR: TLS is enabled but certificate paths are not configured." << std::endl;
		std::cerr << "  Set server_tls_cert and server_tls_key in the database settings," << std::endl;
		std::cerr << "  or run Setup-TLS.ps1 to configure TLS certificates." << std::endl;
		std::cerr << "  To disable TLS, set server_tls_mode to NoSecurity." << std::endl;
		return false;
	}

	if( !fs::exists( m_CertPath ) )
	{
		std::cerr << "TLS ERROR: Certificate file not found: " << m_CertPath << std::endl;
		return false;
	}

	if( !fs::exists( m_KeyPath ) )
	{
		std::cerr << "TLS ERROR: Private key file not found: " << m_KeyPath << std::endl;
		return false;
	}

	LogCertExpiry( m_CertPath );

	m_App.ssl_file( m_CertPath, m_KeyPath );

	// Track cert modification time for auto-reload
	std::error_code ec;
	m_LastCertModTime = fs::last_write_time( m_CertPath, ec );

	std::cout << "TLS: Configured with cert=" << m_CertPath << " key=" << m_KeyPath << std::endl;
	return true;
#else
	if( m_Secure )
	{
		std::cerr << "TLS ERROR: TLS requested but CROW_ENABLE_SSL is not compiled in." << std::endl;
		return false;
	}
	return true;
#endif
}

bool CrowListener::ReloadTLS()
{
#ifdef CROW_ENABLE_SSL
	if( !m_Secure )
	{
		std::cout << "TLS reload skipped — TLS is not enabled." << std::endl;
		return true;
	}

	std::cout << "TLS: Reloading certificate..." << std::endl;

	LogCertExpiry( m_CertPath );

	// Reload the SSL context on the underlying ASIO ssl_context
	// New connections will use the updated certificate
	try
	{
		auto* sslCtx = SSL_CTX_new( TLS_server_method() );
		if( !sslCtx )
		{
			std::cerr << "TLS reload failed: unable to create new SSL context." << std::endl;
			return false;
		}

		if( SSL_CTX_use_certificate_chain_file( sslCtx, m_CertPath.c_str() ) != 1 )
		{
			std::cerr << "TLS reload failed: unable to load certificate." << std::endl;
			SSL_CTX_free( sslCtx );
			return false;
		}

		if( SSL_CTX_use_PrivateKey_file( sslCtx, m_KeyPath.c_str(), SSL_FILETYPE_PEM ) != 1 )
		{
			std::cerr << "TLS reload failed: unable to load private key." << std::endl;
			SSL_CTX_free( sslCtx );
			return false;
		}

		SSL_CTX_free( sslCtx );

		// Validated successfully — now do a graceful server restart
		std::cout << "TLS: Certificate validated, restarting server..." << std::endl;
		m_App.stop();
		if( m_ServerThread.joinable() )
			m_ServerThread.join();

		m_App.ssl_file( m_CertPath, m_KeyPath );

		std::string bindAddr = m_Hostname;
		if( bindAddr == "localhost" ) bindAddr = "127.0.0.1";
		else if( bindAddr == "+" || bindAddr == "*" || bindAddr == "0.0.0.0" ) bindAddr = "0.0.0.0";

		m_App.bindaddr( bindAddr ).port( m_Port );
		m_ServerThread = std::thread( [this]()
		{
			m_App.loglevel( crow::LogLevel::Warning );
			m_App.multithreaded().run();
		});

		// Update tracked modification time
		std::error_code ec;
		m_LastCertModTime = std::filesystem::last_write_time( m_CertPath, ec );

		std::cout << "TLS: Certificate reloaded successfully." << std::endl;
		return true;
	}
	catch( const std::exception& e )
	{
		std::cerr << "TLS reload failed: " << e.what() << std::endl;
		return false;
	}
#else
	std::cout << "TLS reload skipped — CROW_ENABLE_SSL not compiled in." << std::endl;
	return false;
#endif
}

void CrowListener::CertMonitorLoop()
{
#ifdef CROW_ENABLE_SSL
	namespace fs = std::filesystem;

	while( m_CertMonitorRunning.load() )
	{
		// Check every 12 hours
		for( int i = 0; i < 12 * 60 && m_CertMonitorRunning.load(); i++ )
		{
			std::this_thread::sleep_for( std::chrono::minutes(1) );
		}

		if( !m_CertMonitorRunning.load() )
			break;

		std::error_code ec;
		auto currentModTime = fs::last_write_time( m_CertPath, ec );
		if( ec )
			continue;

		if( currentModTime != m_LastCertModTime )
		{
			std::cout << "TLS: Certificate file changed on disk, triggering reload..." << std::endl;
			ReloadTLS();
		}
	}
#endif
}

void CrowListener::Start()
{
	if( !ConfigureSSL() )
	{
		std::cerr << "Server failed to start due to TLS configuration error." << std::endl;
		return;
	}

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

	printf( "Crow server started on %s:%d (%s)\n", m_Hostname.c_str(), m_Port, m_Secure ? "HTTPS" : "HTTP" );

	// Start cert file monitor if TLS is active
	if( m_Secure && !m_CertPath.empty() )
	{
		m_CertMonitorRunning = true;
		m_CertMonitorThread = std::thread( &CrowListener::CertMonitorLoop, this );
	}
}

void CrowListener::Stop()
{
	// Stop cert monitor
	m_CertMonitorRunning = false;
	if( m_CertMonitorThread.joinable() )
		m_CertMonitorThread.join();

	m_App.stop();

	if( m_ServerThread.joinable() )
	{
		m_ServerThread.join();
	}
}