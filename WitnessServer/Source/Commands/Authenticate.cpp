#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "sodium.h"

#include <iostream>
#include <chrono>

using namespace web::json;
using namespace web::http::client;

string_t GetRandomToken()
{
	unsigned char TokenBytes[ 32 ];
	char TokenString[ (2*sizeof(TokenBytes))+1 ];

	randombytes_buf( TokenBytes, sizeof(TokenBytes) );

	sodium_bin2hex( TokenString, sizeof(TokenString), TokenBytes, sizeof(TokenBytes) );

	string TokenStringASCII = TokenString;
	return string_t( TokenStringASCII.begin(), TokenStringASCII.end() );
}

string_t GetHashedPasswordKey_Algorithm0( const string_t& Username, const string_t Password )
{
	string_t CombinedUsernamePassword = Username;
	CombinedUsernamePassword += _T(":");
	CombinedUsernamePassword += Password;

	char HashedPassword[ crypto_pwhash_STRBYTES ];

	if( crypto_pwhash_str(HashedPassword, (const char*)CombinedUsernamePassword.c_str(), sizeof(TCHAR) * CombinedUsernamePassword.size(), crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE ) != 0 )
	{
		throw "Out of memory";
	}

	string KeyStr = HashedPassword;

	return string_t( KeyStr.begin(), KeyStr.end() );
}

bool CheckHashedPasswordKey_Algorithm0( const string_t& Key, const string_t& Username, const string_t Password )
{
	string_t CombinedUsernamePassword = Username;
	CombinedUsernamePassword += _T(":");
	CombinedUsernamePassword += Password;

	string KeyASCII( Key.begin(), Key.end() );
	
	if( crypto_pwhash_str_verify( KeyASCII.c_str(), (const char*)CombinedUsernamePassword.c_str(), sizeof(TCHAR) * CombinedUsernamePassword.size() ) != 0 )
	{
		return false;
	}

	return true;
}

void OfflineCreationForFirstUser( const GlobalContext& Context )
{
	string_t Username;
	string_t Password;

	bool Success = false;

	{
		SQLiteDatabaseQueryInstance GetUserCount( Context.Database, _T("GetUserCount") );

		
		int Result = GetUserCount->Execute( 
			[&Success]( const SQLiteDatabaseQuery& query )
			{
				if( query.GetColumnValueInt(0) == 0 )
				{
					Success = true;
				}

				return true;
			} 
		);
	}

	if( Success )
	{
		tcout << _T("No user exists, you need to create one.") << endl;
		tcout << _T("Username: ");
		getline( tcin, Username );

		tcout << _T("Password: ");
		SetStdinEcho( false );
		getline( tcin, Password );
		SetStdinEcho( true );

		tcout << endl << _T("Hashing password...") << endl;

		string_t Hash = GetHashedPasswordKey_Algorithm0( Username, Password );

		tcout << _T("Storing password...") << endl;

		{
			SQLiteDatabaseQueryInstance CreateUser( Context.Database, _T("CreateUser") );

			string_t UsernameLC = Username;

			std::transform(UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower);

			CreateUser->Bind( "@Username", UsernameLC.c_str() );
			CreateUser->Bind( "@PasswordHash", Hash.c_str() );
			CreateUser->Bind( "@HashMethod", 0 );

			CreateUser->Execute( nullptr );
		}

		tcout << _T("User ready to use.") << endl;
	}
}

void Command_Authenticate::OnMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	if( ChildPath.size() == 1 && IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("login") ) == 0 )
		{
			OnLoginMessage( Context, Message, CurrentCommand, ChildPath, IsPost );
		}
		else if( Command.compare( _T("logout") ) == 0 )
		{
			OnLogoutMessage( Context, Message, CurrentCommand, ChildPath, IsPost );
		}
		else if( Command.compare( _T("profile") ) == 0 )
		{
			OnGetProfileMessage( Context, Message, CurrentCommand, ChildPath, IsPost );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}

void Command_Authenticate::OnLoginMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	//DO NOT CHECK CSRF HERE

	auto Packet = Message.extract_json().get();

	string_t Errors;
	
	bool Success = true;
	string_t Username;
	string_t Password;

	Success &= GetJsonField( Packet, _T("username"), Username, Errors );
	Success &= GetJsonField( Packet, _T("password"), Password, Errors );

	std::transform(Username.begin(), Username.end(), Username.begin(), ::tolower);

	if( !Success )
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	{
		SQLiteDatabaseQueryInstance FindUser( Context.Database, _T("FindUser") );
		FindUser->Bind( "@Username", Username.c_str() );

		int PasswordAlgorithm;
		string_t PasswordHash;

		bool Success = false;
		int Result = FindUser->Execute( 
			[&Success,&PasswordHash,&PasswordAlgorithm]( const SQLiteDatabaseQuery& query )
			{
				PasswordHash = query.GetColumnValueText( 1 );
				PasswordAlgorithm = query.GetColumnValueInt( 2 );
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
				Message.reply( status_codes::Unauthorized, _T("Unsupported password hash algorithm") );
				return;
			}

			if( !Result )
			{
				Message.reply( status_codes::Unauthorized );
				return;
			}
		}
		else
		{
			Message.reply( status_codes::Unauthorized );
			return;
		}
	}

	{
		string_t SessionToken = GetRandomToken();
		string_t CSRFToken = GetRandomToken();

		auto Now = chrono::system_clock::now().time_since_epoch();
		auto UTCTimeNow = chrono::duration_cast<std::chrono::seconds>(Now).count();

		SQLiteDatabaseQueryInstance CreateSession( Context.Database, _T("CreateSession") );
		CreateSession->Bind( "@SessionToken", SessionToken.c_str() );
		CreateSession->Bind( "@CSRFToken", CSRFToken.c_str() );
		CreateSession->Bind( "@Username", Username.c_str() );
		CreateSession->Bind( "@LastUsed", (int64_t)UTCTimeNow );
		
		CreateSession->Execute( nullptr );
		
		http_response Response;
		Response.set_status_code( status_codes::OK );
		Response.headers().add( _T("Set-Cookie"), _T("SessionToken=") + SessionToken + _T("; HttpOnly; Path=/") );

		json::value ResponseBody;

		Response.set_body( ResponseBody );

		Message.reply( Response );
	}	
}

void Command_Authenticate::OnLogoutMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( !IsAuthenticated( Context, Message, Packet, true ) )
	{
		return;
	}

	string_t SessionToken = GetSessionToken( Message );

	SQLiteDatabaseQueryInstance DeleteSession( Context.Database, _T("DeleteSession") );
	DeleteSession->Bind( "@SessionToken", SessionToken.c_str() );
	DeleteSession->Bind( "@SessionToken", SessionToken.c_str() );
		
	DeleteSession->Execute( nullptr );

	http_response Response;
	Response.set_status_code( status_codes::OK );
	Response.headers().add( _T("Set-Cookie"), _T("SessionToken=; Max-Age=0") );

	json::value ResponseBody;

	Response.set_body( ResponseBody );

	Message.reply( Response );
}

void Command_Authenticate::OnGetProfileMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	//DO NOT CHECK CSRF HERE

	auto Packet = Message.extract_json().get();

	string_t SessionToken = GetSessionToken( Message );

	SQLiteDatabaseQueryInstance FindSession( Context.Database, _T("FindSession") );
	FindSession->Bind( "@SessionToken", SessionToken.c_str() );

	string_t Username;
	string_t CSRFToken;

	bool Success = false;
	int Result = FindSession->Execute( 
		[&Success,&Username,&CSRFToken]( const SQLiteDatabaseQuery& query )
		{
			CSRFToken = query.GetColumnValueText( 1 );
			Username = query.GetColumnValueText( 2 );
			Success = true;
				
			return true;
		} 
	);

	if( Success )
	{
		json::value ResponseBody;
		ResponseBody[_T("csrf")] = json::value(CSRFToken);
		ResponseBody[_T("username")] = json::value(Username);

		Message.reply( status_codes::OK, ResponseBody );
	}
	else
	{
		http_response Response;
		Response.set_status_code( status_codes::Unauthorized );
		Response.headers().add( _T("Set-Cookie"), _T("SessionToken=; Max-Age=0") );

		json::value ResponseBody;

		Response.set_body( ResponseBody );

		Message.reply( Response );

	}
}

string_t Command_Authenticate::GetSessionToken( const http_request& Message )
{
	string_t SessionToken;
	const http_headers& Headers = Message.headers();
	
	for ( auto CookiesIter = Headers.find(_T("Cookie")); CookiesIter != Headers.end(); ++CookiesIter )
	{
		auto CookieSplit = SplitString( CookiesIter->second, _T(";") );
		for (auto Cookie : CookieSplit)
		{
			auto TokenSplit = SplitString( Cookie, _T("=") );
			if( TokenSplit.size() == 2 )
			{
				string_t Left = Trim(TokenSplit[0]);
				string_t Right = Trim(TokenSplit[1]);

				if( Left.compare(_T("SessionToken")) == 0 )
				{
					SessionToken = Right;
				}
			}
		}
		
	}

	return SessionToken;
}

bool Command_Authenticate::IsAuthenticated( const GlobalContext& Context, http_request& Message, const json::value& Packet, bool RequireCSRF )
{
	string_t Errors;
	
	bool Success = true;
	string_t CSRF;

	Success &= GetJsonField( Packet, _T("csrf"), CSRF, Errors );

	string_t SessionToken = GetSessionToken( Message );

	{
		if( RequireCSRF )
		{
			SQLiteDatabaseQueryInstance VerifySessionAndCSRF( Context.Database, _T("VerifySessionAndCSRF") );
			VerifySessionAndCSRF->Bind( "@SessionToken", SessionToken.c_str() );
			VerifySessionAndCSRF->Bind( "@CSRFToken", CSRF.c_str() );
		
			int Count = VerifySessionAndCSRF->Execute( nullptr );
			if( Count != 1 )
			{
				Message.reply( status_codes::BadRequest );
				return false;
			}
		}
		else
		{
			SQLiteDatabaseQueryInstance VerifySession( Context.Database, _T("VerifySession") );
			VerifySession->Bind( "@SessionToken", SessionToken.c_str() );
		
			int Count = VerifySession->Execute( nullptr );
			if( Count != 1 )
			{
				Message.reply( status_codes::BadRequest );
				return false;
			}
		}
	}

	return true;
}