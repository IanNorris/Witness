#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"

#include <iostream>

using namespace web::json;
using namespace web::http::client;

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

	if(		tstrcmp( Username.c_str(), _T("admin") ) == 0
		&&	tstrcmp( Username.c_str(), _T("admin") ) == 0 )
	{
		SQLiteDatabaseQueryInstance GetUserCount( Context->Database, _T("GetUserCount") );

		bool Success = false;
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
		if( Success )
		{
			Message.reply( status_codes::OK );
			return;
		}
	}

	SQLiteDatabaseQueryInstance FindUser( Context->Database, _T("FindUser") );
	SQLiteDatabaseQueryInstance FindUserWithPassword( Context->Database, _T("FindUserWithPassword") );
	
	FindUser->Bind( "@Username", Username.c_str() );
	
	FindUserWithPassword->Bind( "@Username", Username.c_str() );
	FindUserWithPassword->Bind( "@Password", Password.c_str() );

	int Count = FindUserWithPassword->Execute( nullptr );

	//Registration:
	//Usernames are generated in advance, user claims it with a one-time password
	//Client sends the server its public key, which it stores if user has not been seen before
	//One time password is erased

	//Change of password:
	//Client sends its new public key, and signs the request with its old one

	//Login:

	//Server stores Ed25519 public key for username
	//Client (JS) generates public key from username + password combination
	//Client sends a login packet and gets an PK encrypted packet with a nonce as a response. 
	//Client signs the packet and sends it back

	if( Count == 1 )
	{
		Message.reply( status_codes::OK );
	}
	else
	{
		Message.reply( status_codes::Unauthorized, Errors );
	}

	
}
