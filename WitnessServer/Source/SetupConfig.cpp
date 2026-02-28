#include "SetupConfig.h"
#include "AuthHelpers.h"
#include "SQLite.h"

#include <algorithm>

bool SetupConfig::ApplyToDatabase( const std::shared_ptr<SQLiteDatabase>& DB ) const
{
	auto setSetting = [&DB]( const std::string& name, const std::string& value )
	{
		if( value.empty() ) return;

		SQLiteDatabaseQueryInstance query( DB, "SetSetting" );
		query->Bind( "@Name", name.c_str() );
		query->Bind( "@Value", value.c_str() );
		query->Execute( nullptr );
	};

	// Server settings
	setSetting( "server_hostname", Hostname );
	setSetting( "server_tls_mode", TlsMode );
	setSetting( "server_tls_contact", TlsContact );
	setSetting( "server_tls_cert", TlsCertPath );
	setSetting( "server_tls_key", TlsKeyPath );
	setSetting( "server_root", WebRoot );
	setSetting( "server_cache", CachePath );
	setSetting( "server_startup_mode", StartupMode );

	// Create admin user if credentials provided
	if( !Username.empty() && !Password.empty() )
	{
		std::string usernameLC = Username;
		std::transform( usernameLC.begin(), usernameLC.end(), usernameLC.begin(), ::tolower );

		std::string hash = GetHashedPasswordKey_Algorithm0( usernameLC, Password );

		SQLiteDatabaseQueryInstance createUser( DB, "CreateUser" );
		createUser->Bind( "@Username", usernameLC.c_str() );
		createUser->Bind( "@DisplayName", Username.c_str() );
		createUser->Bind( "@PasswordHash", hash.c_str() );
		createUser->Bind( "@HashMethod", 0 );
		createUser->Bind( "@Enabled", 1 );
		createUser->Bind( "@Admin", 1 );
		createUser->Execute( nullptr );

		std::cout << "Admin user '" << Username << "' created." << std::endl;
	}

	return true;
}
