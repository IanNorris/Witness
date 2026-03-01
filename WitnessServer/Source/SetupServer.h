#pragma once

#define WITNESS_SETUP_VERSION "1.0"

#include "crow.h"
#include "SQLite.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

// Minimal HTTP-only setup wizard served on localhost.
// Used when no admin user exists in the database.
class SetupServer
{
public:
	SetupServer( std::shared_ptr<SQLiteDatabase> Database, const std::string& StaticRoot );
	~SetupServer();

	// Blocks until setup is complete. Returns true if an admin was created.
	bool Run();

private:
	void RegisterRoutes();
	int FindFreePort();
	void OpenBrowser( int Port );

	// API handlers
	void HandleStatus( const crow::request& req, crow::response& res );
	void HandleSettings( const crow::request& req, crow::response& res );
	void HandleApply( const crow::request& req, crow::response& res );
	void HandleElevate( const crow::request& req, crow::response& res );
	void HandleElevateStatus( const crow::request& req, crow::response& res );

	// TLS helpers
	bool GenerateSelfSignedCert( struct SetupConfig& config );
	bool RunLetsEncryptCertbot( struct SetupConfig& config );

	std::string m_PendingConfigPath;  // Path to pending config JSON for elevation

	crow::SimpleApp m_App;
	std::shared_ptr<SQLiteDatabase> m_Database;
	std::string m_StaticRoot;
	std::atomic<bool> m_SetupComplete{ false };
	bool m_HasAdmin = false;
};
