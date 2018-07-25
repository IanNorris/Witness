#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "sodium.h"

#include <iostream>
#include <iomanip>
#include <chrono>

#define CURRENT_HASH_METHOD 0

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

		string_t UsernameLC = Username;
		std::transform(UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower);


		tcout << endl << _T("Hashing password...") << endl;

		string_t Hash = GetHashedPasswordKey_Algorithm0( UsernameLC, Password );

		tcout << _T("Storing password...") << endl;

		{
			SQLiteDatabaseQueryInstance CreateUser( Context.Database, _T("CreateUser") );
			
			CreateUser->Bind( "@Username", UsernameLC.c_str() );
			CreateUser->Bind( "@DisplayName", Username.c_str() );
			CreateUser->Bind( "@PasswordHash", Hash.c_str() );
			CreateUser->Bind( "@HashMethod", CURRENT_HASH_METHOD );
			CreateUser->Bind( "@Enabled", 1 );
			CreateUser->Bind( "@Admin", 1 );

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
		else if( Command.compare( _T("new_user") ) == 0 )
		{
			OnNewUserMessage( Context, Message, CurrentCommand, ChildPath, IsPost );
		}
		else if( Command.compare( _T("change_password") ) == 0 )
		{
			OnChangePasswordMessage( Context, Message, CurrentCommand, ChildPath, IsPost );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}
		
		return;
	}
	if (ChildPath.size() == 1 && !IsPost)
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("admin_enum") ) == 0 )
		{
			OnEnumUsersMessage( Context, Message, CurrentCommand, ChildPath, IsPost );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}
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

	int Enabled = 0;
	int Admin = 0;

	{
		Success = false;

		SQLiteDatabaseQueryInstance FindUserForAuth( Context.Database, _T("FindUserForAuth") );
		FindUserForAuth->Bind( "@Username", Username.c_str() );

		int Result = FindUserForAuth->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				Enabled = query.GetColumnValueInt( 1 );
				Admin = query.GetColumnValueInt( 2 );
				Success = true;
				
				return true;
			} 
		);
	}

	if (!Success || !Enabled)
	{
		Message.reply( status_codes::Unauthorized );
		return;
	}

	{
		string_t SessionToken = GetRandomToken();
		string_t CSRFToken = GetRandomToken();

		auto Now = chrono::system_clock::now().time_since_epoch();
		auto UTCTimeNow = chrono::duration_cast<std::chrono::seconds>(Now).count();


		auto ExpiryTime = chrono::system_clock::now(); // +2 Months + 1 day (to account for timezones)
		ExpiryTime += chrono::seconds(60*60*24*((30*2)+1));

		SQLiteDatabaseQueryInstance CreateSession( Context.Database, _T("CreateSession") );
		CreateSession->Bind( "@SessionToken", SessionToken.c_str() );
		CreateSession->Bind( "@CSRFToken", CSRFToken.c_str() );
		CreateSession->Bind( "@Username", Username.c_str() );
		CreateSession->Bind( "@LastUsed", (int64_t)UTCTimeNow );
		
		CreateSession->Execute( nullptr );
		
		http_response Response;
		Response.set_status_code( status_codes::OK );

		string_t PortName = to_wstring(Port);
		string_t SessionTokenValue = _T("SessionToken-") + PortName;

		string_t MaxAge = to_wstring(60*60*24*30*2); // +2 Months
		Response.headers().add( _T("Set-Cookie"), SessionTokenValue + _T("=") + SessionToken + _T("; HttpOnly; Path=/; max-age=") + MaxAge + _T(";") );

		json::value ResponseBody;

		Response.set_body( ResponseBody );

		Message.reply( Response );
	}	
}

void Command_Authenticate::OnLogoutMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( !IsAuthenticated( Context, Message, Packet, Action::ReadWrite, Privilege::Normal ) )
	{
		return;
	}

	string_t SessionToken = GetSessionToken( Message, Port );

	SQLiteDatabaseQueryInstance DeleteSession( Context.Database, _T("DeleteSession") );
	DeleteSession->Bind( "@SessionToken", SessionToken.c_str() );
	DeleteSession->Bind( "@SessionToken", SessionToken.c_str() );
		
	DeleteSession->Execute( nullptr );

	http_response Response;
	Response.set_status_code( status_codes::OK );

	string_t PortName = to_wstring(Port);
	string_t SessionTokenValue = _T("SessionToken-") + PortName + _T("=; Max-Age=0");

	Response.headers().add( _T("Set-Cookie"), SessionTokenValue );

	json::value ResponseBody;

	Response.set_body( ResponseBody );

	Message.reply( Response );
}

void Command_Authenticate::OnGetProfileMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	//DO NOT CHECK CSRF HERE

	auto Packet = Message.extract_json().get();

	string_t SessionToken = GetSessionToken( Message, Port );

	string_t Username;
	string_t CSRFToken;

	bool Success = false;

	{
		SQLiteDatabaseQueryInstance FindSession( Context.Database, _T("FindSession") );
		FindSession->Bind( "@SessionToken", SessionToken.c_str() );

		int Result = FindSession->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				CSRFToken = query.GetColumnValueText( 1 );
				Username = query.GetColumnValueText( 2 );
				Success = true;
				
				return true;
			} 
		);
	}

	string_t DisplayName;
	int Enabled = 0;
	int Admin = 0;

	{
		Success = false;

		SQLiteDatabaseQueryInstance FindUserForAuth( Context.Database, _T("FindUserForAuth") );
		FindUserForAuth->Bind( "@Username", Username.c_str() );

		int Result = FindUserForAuth->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				DisplayName = query.GetColumnValueText( 0 );
				Enabled = query.GetColumnValueInt( 1 );
				Admin = query.GetColumnValueInt( 2 );
				Success = true;
				
				return true;
			} 
		);
	}
	
	if( Success && Enabled )
	{
		json::value ResponseBody;
		ResponseBody[_T("csrf")] = json::value(CSRFToken);
		ResponseBody[_T("username")] = json::value(Username);
		ResponseBody[_T("admin")] = json::value(Admin);
		ResponseBody[_T("displayName")] = json::value(DisplayName);

		Message.reply( status_codes::OK, ResponseBody );
	}
	else
	{
		string_t PortName = to_wstring(Port);
		string_t SessionTokenValue = _T("SessionToken-") + PortName + _T("=; Max-Age=0");

		http_response Response;
		Response.set_status_code( status_codes::Unauthorized );
		Response.headers().add( _T("Set-Cookie"), SessionTokenValue );

		json::value ResponseBody;

		Response.set_body( ResponseBody );

		Message.reply( Response );
	}
}

string_t Command_Authenticate::GetSessionToken( const http_request& Message, uint16_t PortIn )
{
	string_t PortName = to_wstring(PortIn);
	string_t SessionTokenName = _T("SessionToken-") + PortName;

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

				if( Left.compare(SessionTokenName.c_str()) == 0 )
				{
					SessionToken = Right;
				}
			}
		}
		
	}

	return SessionToken;
}

bool Command_Authenticate::IsAuthenticated( const GlobalContext& Context, http_request& Message, const json::value& Packet, Action ActionType, Privilege RequiredPrivilege )
{
	string_t Errors;
	
	bool Success = true;
	string_t CSRF;

	Success &= GetJsonField( Packet, _T("csrf"), CSRF, Errors );

	string_t SessionToken = GetSessionToken( Message, Context.Port );
	string_t Username;

	{
		bool QuerySuccess = false;

		//Verify CSRF for R/W actions
		if( ActionType == Action::ReadWrite )
		{
			SQLiteDatabaseQueryInstance VerifySessionAndCSRF( Context.Database, _T("VerifySessionAndCSRF") );
			VerifySessionAndCSRF->Bind( "@SessionToken", SessionToken.c_str() );
			VerifySessionAndCSRF->Bind( "@CSRFToken", CSRF.c_str() );
		
			int Count = VerifySessionAndCSRF->Execute( 
				[&]( const SQLiteDatabaseQuery& query )
				{
					Username = query.GetColumnValueText( 2 );
					QuerySuccess = true;
				
					return true;
				} 
			);
			if( Count != 1 || !QuerySuccess )
			{
				Message.reply( status_codes::BadRequest );
				return false;
			}
		}
		else
		{
			SQLiteDatabaseQueryInstance VerifySession( Context.Database, _T("VerifySession") );
			VerifySession->Bind( "@SessionToken", SessionToken.c_str() );
		
			int Count = VerifySession->Execute( 
				[&]( const SQLiteDatabaseQuery& query )
				{
					Username = query.GetColumnValueText( 2 );
					QuerySuccess = true;
				
					return true;
				} 
			);
			if( Count != 1 || !QuerySuccess )
			{
				Message.reply( status_codes::BadRequest );
				return false;
			}
		}
	}

	int Enabled = 0;
	int Admin = 0;

	{
		Success = false;

		SQLiteDatabaseQueryInstance FindUserForAuth( Context.Database, _T("FindUserForAuth") );
		FindUserForAuth->Bind( "@Username", Username.c_str() );

		bool Success = false;
		int Result = FindUserForAuth->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				Enabled = query.GetColumnValueInt( 1 );
				Admin = query.GetColumnValueInt( 2 );
				Success = true;
				
				return true;
			} 
		);
	}

	if ( !Admin && RequiredPrivilege == Privilege::Administrator)
	{
		Message.reply( status_codes::Forbidden );
		return false;
	}

	if( !Enabled )
	{
		Message.reply( status_codes::Forbidden );
		return false;
	}

	return true;
}

void Command_Authenticate::OnEnumUsersMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( !IsAuthenticated( Context, Message, Packet, Action::Read, Privilege::Administrator ) )
	{
		return;
	}

	vector<json::value> Array;

	SQLiteDatabaseQueryInstance FindUsers( Context.Database, _T("FindUsers") );

	int Result = FindUsers->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			json::value User;
			User[ _T("username") ] = json::value(query.GetColumnValueText( 0 ));
			User[ _T("displayName") ] = json::value(query.GetColumnValueText( 1 ));
			User[ _T("enabled") ] = json::value(query.GetColumnValueInt( 2 ));
			User[ _T("admin") ] = json::value(query.GetColumnValueInt( 3 ));
			
			Array.push_back(User);

			return true;
		} 
	);

	Message.reply( status_codes::OK, json::value::array(Array) );
}

void Command_Authenticate::OnNewUserMessage(const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost)
{
	auto Packet = Message.extract_json().get();

	if( !IsAuthenticated( Context, Message, Packet, Action::ReadWrite, Privilege::Administrator ) )
	{
		return;
	}

	string_t Errors;
	
	bool Success = true;
	string_t Username;
	
	Success &= GetJsonField( Packet, _T("username"), Username, Errors );

	if (!Success)
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	//Generate password (some letters missing as they can be ambiguous)
	constexpr TCHAR PasswordCharacters[] = _T("ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789!$%@?+-&");
	constexpr int PasswordCharacterCount = (sizeof(PasswordCharacters)-1)/sizeof(PasswordCharacters[0]);
	const int DefaultPasswordLength = 8;
	unsigned char TokenBytes[ DefaultPasswordLength ];
	TCHAR Password[ DefaultPasswordLength+1 ];

	randombytes_buf( TokenBytes, sizeof(TokenBytes) );
	for (int i = 0; i < DefaultPasswordLength; i++)
	{
		Password[i] = PasswordCharacters[ TokenBytes[i]%PasswordCharacterCount ];
	}
	Password[DefaultPasswordLength] = '\0';

	string_t UsernameLC = Username;
	std::transform(UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower);

	string_t Hash = GetHashedPasswordKey_Algorithm0( UsernameLC, Password );

	int64_t RowResult = 0;

	{
		SQLiteDatabaseQueryInstance CreateUser( Context.Database, _T("CreateUser") );
			
		CreateUser->Bind( "@Username", UsernameLC.c_str() );
		CreateUser->Bind( "@DisplayName", Username.c_str() );
		CreateUser->Bind( "@PasswordHash", Hash.c_str() );
		CreateUser->Bind( "@HashMethod", CURRENT_HASH_METHOD );
		CreateUser->Bind( "@Enabled", 1 );
		CreateUser->Bind( "@Admin", 0 );

		if (CreateUser->Execute(nullptr) < 0)
		{
			json::value Data;
			Data[ _T("errorMessage") ] = json::value(CreateUser->GetLastError());

			Message.reply( status_codes::BadRequest, Data );
			return;
		}

		RowResult = CreateUser->GetLastInsertionId();
	}

	json::value Data;
	Data[ _T("id") ] = json::value(RowResult);
	Data[ _T("username") ] = json::value(UsernameLC);
	Data[ _T("displayName") ] = json::value(Username);	
	Data[ _T("password") ] = json::value(Password);

	if( RowResult > 0 )
	{
		Message.reply( status_codes::OK, Data );
	}
	else
	{
		Message.reply( status_codes::BadRequest );
	}
}

void Command_Authenticate::OnChangePasswordMessage(const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost)
{
	auto Packet = Message.extract_json().get();

	if( !IsAuthenticated( Context, Message, Packet, Action::ReadWrite, Privilege::Administrator ) )
	{
		return;
	}
}
