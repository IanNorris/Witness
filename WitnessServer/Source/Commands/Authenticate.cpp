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

void OfflineCreationForFirstUser( const unique_ptr<GlobalContext>& Context )
{
	string_t Username;
	string_t Password;

	bool Success = false;

	{
		SQLiteDatabaseQueryInstance GetUserCount( Context->Database, _T("GetUserCount") );

		
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
			SQLiteDatabaseQueryInstance CreateUser( Context->Database, _T("CreateUser") );

			CreateUser->Bind( "@Username", Username.c_str() );
			CreateUser->Bind( "@PasswordHash", Hash.c_str() );
			CreateUser->Bind( "@HashMethod", 0 );

			CreateUser->Execute( nullptr );
		}

		tcout << _T("User ready to use.") << endl;
	}
}

void Command_Authenticate::OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto LoginPacket = Message.extract_json().get();

	string_t Errors;
	
	bool Success = true;
	string_t Username;
	string_t Password;

	Success &= GetJsonField( LoginPacket, _T("username"), Username, Errors );
	Success &= GetJsonField( LoginPacket, _T("password"), Password, Errors );

	if( !Success )
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	{
		SQLiteDatabaseQueryInstance FindUser( Context->Database, _T("FindUser") );
		FindUser->Bind( "@Username", Username.c_str() );

		int PasswordAlgorithm;
		string_t UsernameDB;
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

		SQLiteDatabaseQueryInstance CreateSession( Context->Database, _T("CreateSession") );
		CreateSession->Bind( "@SessionToken", SessionToken.c_str() );
		CreateSession->Bind( "@CSRFToken", CSRFToken.c_str() );
		CreateSession->Bind( "@Username", Username.c_str() );
		CreateSession->Bind( "@LastUsed", (int64_t)UTCTimeNow );
		
		CreateSession->Execute( nullptr );
		
		http_response Response;
		Response.set_status_code( status_codes::OK );
		Response.headers().add( _T("set-cookie"), _T("SessionToken=") + SessionToken + _T("CSRFToken=") + CSRFToken );

		json::value ResponseBody;

		Response.set_body( ResponseBody );

		Message.reply( Response );
	}
	
	
}
