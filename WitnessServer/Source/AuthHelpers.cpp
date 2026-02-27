#include "AuthHelpers.h"
#include "GlobalContext.h"
#include "sodium.h"

#include <iostream>

#define CURRENT_HASH_METHOD 0

StringT GetRandomToken()
{
	unsigned char TokenBytes[ 32 ];
	char TokenString[ (2*sizeof(TokenBytes))+1 ];

	randombytes_buf( TokenBytes, sizeof(TokenBytes) );

	sodium_bin2hex( TokenString, sizeof(TokenString), TokenBytes, sizeof(TokenBytes) );

	std::string TokenStringASCII = TokenString;
	return StringT( TokenStringASCII.begin(), TokenStringASCII.end() );
}

StringT GetHashedPasswordKey_Algorithm0( const StringT& Username, const StringT Password )
{
	std::string CombinedUsernamePassword = StringToAnsi(Username);
	CombinedUsernamePassword += ":";
	CombinedUsernamePassword += StringToAnsi(Password);

	char HashedPassword[ crypto_pwhash_STRBYTES ];

	if( crypto_pwhash_str(HashedPassword, (const char*)CombinedUsernamePassword.c_str(), CombinedUsernamePassword.size(), crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE ) != 0 )
	{
		throw "Out of memory";
	}

	std::string KeyStr = HashedPassword;

	return StringT( KeyStr.begin(), KeyStr.end() );
}

bool CheckHashedPasswordKey_Algorithm0( const StringT& Key, const StringT& Username, const StringT Password )
{
	std::string CombinedUsernamePassword = StringToAnsi(Username);
	CombinedUsernamePassword += ":";
	CombinedUsernamePassword += StringToAnsi(Password);

	std::string KeyASCII = StringToAnsi(Key);
	
	if( crypto_pwhash_str_verify( KeyASCII.c_str(), (const char*)CombinedUsernamePassword.c_str(), CombinedUsernamePassword.size() ) != 0 )
	{
		return false;
	}

	return true;
}

void OfflineCreationForFirstUser( const GlobalContext& Context )
{
	StringT Username;
	StringT Password;

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
		std::tcout << _T("No user exists, you need to create one.") << std::endl;
		std::tcout << _T("Username: ");
		getline(std::tcin, Username );

		std::tcout << _T("Password: ");
		SetStdinEcho( false );
		getline(std::tcin, Password );
		SetStdinEcho( true );

		StringT UsernameLC = Username;
		std::transform(UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower);


		std::tcout << std::endl << _T("Hashing password...") << std::endl;

		StringT Hash = GetHashedPasswordKey_Algorithm0( UsernameLC, Password );

		std::tcout << _T("Storing password...") << std::endl;

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

		std::tcout << _T("User ready to use.") << std::endl;
	}
}
