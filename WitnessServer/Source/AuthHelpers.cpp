#include "AuthHelpers.h"
#include "GlobalContext.h"
#include "sodium.h"

#include <iostream>

#define CURRENT_HASH_METHOD 0

std::string GetRandomToken()
{
	unsigned char TokenBytes[ 32 ];
	char TokenString[ (2*sizeof(TokenBytes))+1 ];

	randombytes_buf( TokenBytes, sizeof(TokenBytes) );

	sodium_bin2hex( TokenString, sizeof(TokenString), TokenBytes, sizeof(TokenBytes) );

	std::string TokenStringASCII = TokenString;
	return std::string( TokenStringASCII.begin(), TokenStringASCII.end() );
}

std::string GetHashedPasswordKey_Algorithm0( const std::string& Username, const std::string Password )
{
	std::string CombinedUsernamePassword = Username;
	CombinedUsernamePassword += ":";
	CombinedUsernamePassword += Password;

	char HashedPassword[ crypto_pwhash_STRBYTES ];

	if( crypto_pwhash_str(HashedPassword, (const char*)CombinedUsernamePassword.c_str(), CombinedUsernamePassword.size(), crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE ) != 0 )
	{
		throw "Out of memory";
	}

	std::string KeyStr = HashedPassword;

	return std::string( KeyStr.begin(), KeyStr.end() );
}

bool CheckHashedPasswordKey_Algorithm0( const std::string& Key, const std::string& Username, const std::string Password )
{
	std::string CombinedUsernamePassword = Username;
	CombinedUsernamePassword += ":";
	CombinedUsernamePassword += Password;

	std::string KeyASCII = Key;
	
	if( crypto_pwhash_str_verify( KeyASCII.c_str(), (const char*)CombinedUsernamePassword.c_str(), CombinedUsernamePassword.size() ) != 0 )
	{
		return false;
	}

	return true;
}

void OfflineCreationForFirstUser( const GlobalContext& Context )
{
	std::string Username;
	std::string Password;

	bool Success = false;

	{
		SQLiteDatabaseQueryInstance GetUserCount( Context.Database, "GetUserCount" );

		
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
		std::cout << "No user exists, you need to create one." << std::endl;
		std::cout << "Username: ";
		getline(std::cin, Username );

		std::cout << "Password: ";
		SetStdinEcho( false );
		getline(std::cin, Password );
		SetStdinEcho( true );

		std::string UsernameLC = Username;
		std::transform(UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower);


		std::cout << std::endl << "Hashing password..." << std::endl;

		std::string Hash = GetHashedPasswordKey_Algorithm0( UsernameLC, Password );

		std::cout << "Storing password..." << std::endl;

		{
			SQLiteDatabaseQueryInstance CreateUser( Context.Database, "CreateUser" );
			
			CreateUser->Bind( "@Username", UsernameLC.c_str() );
			CreateUser->Bind( "@DisplayName", Username.c_str() );
			CreateUser->Bind( "@PasswordHash", Hash.c_str() );
			CreateUser->Bind( "@HashMethod", CURRENT_HASH_METHOD );
			CreateUser->Bind( "@Enabled", 1 );
			CreateUser->Bind( "@Admin", 1 );

			CreateUser->Execute( nullptr );
		}

		std::cout << "User ready to use." << std::endl;
	}
}
