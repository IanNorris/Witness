#include "SetupConfig.h"
#include "AuthHelpers.h"
#include "SQLite.h"

#include <Log.h>
#include <algorithm>

bool SetupConfig::ApplyToDatabase( const std::shared_ptr<SQLiteDatabase>& DB ) const
{
	auto setSetting = [&DB]( const std::string& name, const std::string& value, bool allowEmpty = false )
	{
		if( value.empty() && !allowEmpty ) return;

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
	setSetting( "server_cache", CachePath );
	setSetting( "server_startup_mode", StartupMode );

	// Detection settings -- always write even if empty (to allow clearing)
	setSetting( "detection_backend", DetectionBackend, true );
	setSetting( "detection_provider", DetectionProvider, true );
	setSetting( "detection_confidence", DetectionConfidence, true );
	setSetting( "detection_max_fps", DetectionMaxFPS, true );
	setSetting( "cudnn_path", CudnnPath, true );
	setSetting( "clip_cleanup_enabled", ClipCleanupEnabled, true );
	setSetting( "clip_retention_days", ClipRetentionDays, true );

	// Create or update admin user if credentials provided
	if( !Username.empty() && !Password.empty() )
	{
		std::string usernameLC = Username;
		std::transform( usernameLC.begin(), usernameLC.end(), usernameLC.begin(), ::tolower );

		std::string hash = GetHashedPasswordKey_Algorithm0( usernameLC, Password );

		// Check if user already exists
		bool userExists = false;
		{
			SQLiteDatabaseQueryInstance findUser( DB, "FindUser" );
			findUser->Bind( "@Username", usernameLC.c_str() );
			findUser->Execute( [&userExists]( const SQLiteDatabaseQuery& q )
			{
				userExists = true;
				return true;
			});
		}

		if( userExists )
		{
			// Update password for existing user
			SQLiteDatabaseQueryInstance update( DB, "SetUserPassword" );
			update->Bind( "@Username", usernameLC.c_str() );
			update->Bind( "@PasswordHash", hash.c_str() );
			update->Bind( "@HashMethod", 0 );
			update->Execute( nullptr );

			LOG_INFO( "Admin user '%s' password updated.", Username.c_str() );
		}
		else
		{
			// Create new admin user
			SQLiteDatabaseQueryInstance createUser( DB, "CreateUser" );
			createUser->Bind( "@Username", usernameLC.c_str() );
			createUser->Bind( "@DisplayName", Username.c_str() );
			createUser->Bind( "@PasswordHash", hash.c_str() );
			createUser->Bind( "@HashMethod", 0 );
			createUser->Bind( "@Enabled", 1 );
			createUser->Bind( "@Admin", 1 );
			createUser->Execute( nullptr );

			LOG_INFO( "Admin user '%s' created.", Username.c_str() );
		}
	}

	return true;
}
