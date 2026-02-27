#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "AuthHelpers.h"
#include "sodium.h"

// ===== Auth Handlers =====

void CrowListener::HandleAuthLogin( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("username") || !body.has("password") )
	{
		res.code = 400;
		res.body = "Missing username or password";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	std::string PasswordStr = body["password"].s();

	std::string Username( UsernameStr.begin(), UsernameStr.end() );
	std::string Password( PasswordStr.begin(), PasswordStr.end() );

	std::transform( Username.begin(), Username.end(), Username.begin(), ::tolower );

	int UserUID = -1;

	{
		SQLiteDatabaseQueryInstance FindUser( m_GlobalContext->Database, "FindUser" );
		FindUser->Bind( "@Username", Username.c_str() );

		int PasswordAlgorithm = 0;
		std::string PasswordHash;

		bool Success = false;
		FindUser->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				UserUID = query.GetColumnValueInt( 0 );
				PasswordHash = query.GetColumnValueText( 2 );
				PasswordAlgorithm = query.GetColumnValueInt( 3 );
				Success = true;
				return true;
			}
		);

		if( Success )
		{
			bool Result = false;
			switch( PasswordAlgorithm )
			{
			case 0:
				Result = CheckHashedPasswordKey_Algorithm0( PasswordHash, Username, Password );
				break;
			default:
				res.code = 401;
				res.body = "Unsupported password hash algorithm";
				res.end();
				return;
			}

			if( !Result )
			{
				res.code = 401;
				res.end();
				return;
			}
		}
		else
		{
			res.code = 401;
			res.end();
			return;
		}
	}

	int Enabled = 0;
	int Admin = 0;

	{
		bool Success = false;
		SQLiteDatabaseQueryInstance FindUserForAuth( m_GlobalContext->Database, "FindUserForAuth" );
		FindUserForAuth->Bind( "@UserUID", UserUID );

		FindUserForAuth->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				Enabled = query.GetColumnValueInt( 3 );
				Admin = query.GetColumnValueInt( 4 );
				Success = true;
				return true;
			}
		);

		if( !Success || !Enabled )
		{
			res.code = 401;
			res.end();
			return;
		}
	}

	{
		std::string SessionToken = GetRandomToken();
		std::string CSRFToken = GetRandomToken();

		auto Now = std::chrono::system_clock::now().time_since_epoch();
		auto UTCTimeNow = std::chrono::duration_cast<std::chrono::seconds>( Now ).count();

		SQLiteDatabaseQueryInstance CreateSession( m_GlobalContext->Database, "CreateSession" );
		CreateSession->Bind( "@SessionToken", SessionToken.c_str() );
		CreateSession->Bind( "@CSRFToken", CSRFToken.c_str() );
		CreateSession->Bind( "@UserUID", UserUID );
		CreateSession->Bind( "@LastUsed", (int64_t)UTCTimeNow );

		CreateSession->Execute( nullptr );

		std::string PortName = std::to_string( m_Port );
		std::string SessionTokenValue = "SessionToken-" + PortName;

		std::string MaxAge = std::to_string( 60 * 60 * 24 * 30 * 2 ); // +2 Months
		res.add_header( "Set-Cookie", SessionTokenValue + "=" + SessionToken + "; HttpOnly; Path=/; max-age=" + MaxAge + ";" );

		res.set_header( "Content-Type", "application/json" );
		res.body = "{}";
		res.code = 200;
		res.end();
	}
}

void CrowListener::HandleAuthLogout( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::string SessionTokenStr = CrowAuth::GetSessionToken( req, m_Port );
	std::string SessionToken( SessionTokenStr.begin(), SessionTokenStr.end() );

	SQLiteDatabaseQueryInstance DeleteSession( m_GlobalContext->Database, "DeleteSession" );
	DeleteSession->Bind( "@SessionToken", SessionToken.c_str() );
	DeleteSession->Execute( nullptr );

	std::string PortName = std::to_string( m_Port );
	res.add_header( "Set-Cookie", "SessionToken-" + PortName + "=; Max-Age=0" );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleAuthGetProfile( const crow::request& req, crow::response& res )
{
	// DO NOT CHECK CSRF HERE
	std::string SessionTokenStr = CrowAuth::GetSessionToken( req, m_Port );
	std::string SessionToken( SessionTokenStr.begin(), SessionTokenStr.end() );

	int UserUID = -1;
	std::string Username;
	std::string CSRFToken;

	bool SessionFound = false;

	{
		SQLiteDatabaseQueryInstance FindSession( m_GlobalContext->Database, "FindSession" );
		FindSession->Bind( "@SessionToken", SessionToken.c_str() );

		FindSession->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				CSRFToken = query.GetColumnValueText( 1 );
				UserUID = query.GetColumnValueInt( 2 );
				SessionFound = true;
				return true;
			}
		);
	}

	std::string DisplayName;
	int Enabled = 0;
	int Admin = 0;

	bool UserFound = false;

	{
		SQLiteDatabaseQueryInstance FindUserForAuth( m_GlobalContext->Database, "FindUserForAuth" );
		FindUserForAuth->Bind( "@UserUID", UserUID );

		FindUserForAuth->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				Username = query.GetColumnValueText( 1 );
				DisplayName = query.GetColumnValueText( 2 );
				Enabled = query.GetColumnValueInt( 3 );
				Admin = query.GetColumnValueInt( 4 );
				UserFound = true;
				return true;
			}
		);
	}

	if( SessionFound && UserFound && Enabled )
	{
		crow::json::wvalue ResponseBody;
		ResponseBody["csrf"] = CSRFToken;
		ResponseBody["username"] = Username;
		ResponseBody["userUid"] = UserUID;
		ResponseBody["admin"] = Admin;
		ResponseBody["displayName"] = DisplayName;

		res.set_header( "Content-Type", "application/json" );
		res.body = ResponseBody.dump();
		res.code = 200;
		res.end();
	}
	else
	{
		std::string PortName = std::to_string( m_Port );
		res.add_header( "Set-Cookie", "SessionToken-" + PortName + "=; Max-Age=0" );

		res.set_header( "Content-Type", "application/json" );
		res.body = "{}";
		res.code = 401;
		res.end();
	}
}

void CrowListener::HandleAuthEnumUsers( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	struct UserLookup { int UserUID; int ArrayIndex; };

	std::vector<crow::json::wvalue> Array;
	std::vector<UserLookup> LookupArray;

	SQLiteDatabaseQueryInstance FindUsers( m_GlobalContext->Database, "FindUsers" );

	FindUsers->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			crow::json::wvalue User;
			int uid = query.GetColumnValueInt( 0 );
			User["userid"] = uid;
			User["username"] = query.GetColumnValueText( 1 );
			User["displayName"] = query.GetColumnValueText( 2 );
			User["enabled"] = query.GetColumnValueInt( 3 );
			User["admin"] = query.GetColumnValueInt( 4 );

			Array.push_back( std::move( User ) );
			LookupArray.push_back( UserLookup{ uid, (int)Array.size() - 1 } );

			return true;
		}
	);

	for( auto& Lookup : LookupArray )
	{
		SQLiteDatabaseQueryInstance SelectGroupsForUser( m_GlobalContext->Database, "SelectGroupsForUser" );
		SelectGroupsForUser->Bind( "@User", Lookup.UserUID );

		std::vector<crow::json::wvalue> Groups;

		SelectGroupsForUser->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				int Group = query.GetColumnValueInt( 1 );
				Groups.push_back( Group );
				return true;
			}
		);

		Array[Lookup.ArrayIndex]["groups"] = std::move( Groups );
	}

	crow::json::wvalue Result = std::move( Array );
	res.set_header( "Content-Type", "application/json" );
	res.body = Result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleAuthNewUser( const crow::request& req, crow::response& res )
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

	if( !body.has("username") )
	{
		res.code = 400;
		res.body = "Missing username";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	std::string Username( UsernameStr.begin(), UsernameStr.end() );

	// Generate random password
	constexpr char PasswordCharacters[] = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789!$%@?+-&";
	constexpr int PasswordCharacterCount = sizeof(PasswordCharacters) - 1;
	const int DefaultPasswordLength = 8;
	unsigned char TokenBytes[DefaultPasswordLength];
	char PasswordChars[DefaultPasswordLength + 1];

	randombytes_buf( TokenBytes, sizeof(TokenBytes) );
	for( int i = 0; i < DefaultPasswordLength; i++ )
	{
		PasswordChars[i] = PasswordCharacters[TokenBytes[i] % PasswordCharacterCount];
	}
	PasswordChars[DefaultPasswordLength] = '\0';

	std::string PasswordStr = PasswordChars;
	std::string Password( PasswordStr.begin(), PasswordStr.end() );

	std::string UsernameLC = Username;
	std::transform( UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower );

	std::string Hash = GetHashedPasswordKey_Algorithm0( UsernameLC, Password );

	int64_t RowResult = 0;

	{
		SQLiteDatabaseQueryInstance CreateUser( m_GlobalContext->Database, "CreateUser" );
		CreateUser->Bind( "@Username", UsernameLC.c_str() );
		CreateUser->Bind( "@DisplayName", Username.c_str() );
		CreateUser->Bind( "@PasswordHash", Hash.c_str() );
		CreateUser->Bind( "@HashMethod", 0 );
		CreateUser->Bind( "@Enabled", 1 );
		CreateUser->Bind( "@Admin", 0 );

		if( CreateUser->Execute( nullptr ) < 0 )
		{
			crow::json::wvalue Data;
			Data["errorMessage"] = CreateUser->GetLastError();
			res.set_header( "Content-Type", "application/json" );
			res.body = Data.dump();
			res.code = 400;
			res.end();
			return;
		}

		RowResult = CreateUser->GetLastInsertionId();
	}

	crow::json::wvalue Data;
	Data["id"] = RowResult;
	Data["username"] = UsernameLC;
	Data["displayName"] = UsernameStr;
	Data["password"] = PasswordStr;

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = RowResult > 0 ? 200 : 400;
	res.end();
}

void CrowListener::HandleAuthChangePassword( const crow::request& req, crow::response& res )
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

	// Original code was empty/unimplemented
	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleAuthToggleEnabled( const crow::request& req, crow::response& res )
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

	if( !body.has("username") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	bool Value = body["value"].b();
	std::string Username( UsernameStr.begin(), UsernameStr.end() );

	SQLiteDatabaseQueryInstance SetUserEnabledState( m_GlobalContext->Database, "SetUserEnabledState" );
	SetUserEnabledState->Bind( "@Username", Username.c_str() );
	SetUserEnabledState->Bind( "@Enabled", Value ? 1 : 0 );

	int Result = SetUserEnabledState->Execute(
		[&]( const SQLiteDatabaseQuery& query ) { return true; }
	);

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Result >= 0 ? 200 : 500;
	res.end();
}

void CrowListener::HandleAuthToggleAdmin( const crow::request& req, crow::response& res )
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

	if( !body.has("username") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	bool Value = body["value"].b();
	std::string Username( UsernameStr.begin(), UsernameStr.end() );

	SQLiteDatabaseQueryInstance SetUserAdminState( m_GlobalContext->Database, "SetUserAdminState" );
	SetUserAdminState->Bind( "@Username", Username.c_str() );
	SetUserAdminState->Bind( "@Admin", Value ? 1 : 0 );

	int Result = SetUserAdminState->Execute(
		[&]( const SQLiteDatabaseQuery& query ) { return true; }
	);

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Result >= 0 ? 200 : 500;
	res.end();
}

void CrowListener::HandleAuthSetDisplayName( const crow::request& req, crow::response& res )
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

	if( !body.has("username") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	std::string DisplayNameStr = body["value"].s();
	std::string Username( UsernameStr.begin(), UsernameStr.end() );
	std::string DisplayName( DisplayNameStr.begin(), DisplayNameStr.end() );

	SQLiteDatabaseQueryInstance SetUserDisplayName( m_GlobalContext->Database, "SetUserDisplayName" );
	SetUserDisplayName->Bind( "@Username", Username.c_str() );
	SetUserDisplayName->Bind( "@DisplayName", DisplayName.c_str() );

	int Result = SetUserDisplayName->Execute(
		[&]( const SQLiteDatabaseQuery& query ) { return true; }
	);

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Result >= 0 ? 200 : 500;
	res.end();
}

void CrowListener::HandleAuthSetUserGroups( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int LoggedInUserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( LoggedInUserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("userid") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	int UserUID = (int)body["userid"].i();

	std::vector<int> UserGroupsRequested;
	for( auto& Element : body["value"] )
	{
		UserGroupsRequested.push_back( (int)Element.i() );
	}

	std::vector<int> UserGroupsCurrent;

	SQLiteDatabaseQueryInstance SelectGroupsForUser( m_GlobalContext->Database, "SelectGroupsForUser" );
	SelectGroupsForUser->Bind( "@User", UserUID );

	SelectGroupsForUser->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			int Group = query.GetColumnValueInt( 1 );
			UserGroupsCurrent.push_back( Group );
			return true;
		}
	);

	for( int Value : UserGroupsRequested )
	{
		if( find( UserGroupsCurrent.begin(), UserGroupsCurrent.end(), Value ) == UserGroupsCurrent.end() )
		{
			SQLiteDatabaseQueryInstance CreateMapping( m_GlobalContext->Database, "CreateUserGroupMapping" );
			CreateMapping->Bind( "@UserUID", UserUID );
			CreateMapping->Bind( "@Group", Value );
			CreateMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	for( int Value : UserGroupsCurrent )
	{
		if( find( UserGroupsRequested.begin(), UserGroupsRequested.end(), Value ) == UserGroupsRequested.end() )
		{
			SQLiteDatabaseQueryInstance DeleteMapping( m_GlobalContext->Database, "DeleteUserGroupMapping" );
			DeleteMapping->Bind( "@UserUID", UserUID );
			DeleteMapping->Bind( "@Group", Value );
			DeleteMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}