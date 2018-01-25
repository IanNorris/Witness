#include "Common.h"
#include "SQLite.h"

namespace Database
{
	string InitializationScript = R"RAW(
		CREATE TABLE IF NOT EXISTS User(
			Username		CHAR(64)							NOT NULL,
			PasswordHash	CHAR(128)							NOT NULL,
			HashMethod		INT									NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS UserIndex ON User (Username);

		CREATE TABLE IF NOT EXISTS Session(
			SessionToken	CHAR(64)							NOT NULL,
			CSRFToken		CHAR(64)							NOT NULL,
			Username		CHAR(64)							NOT NULL,
			LastUsed		DATETIME							NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS SessionIndex ON Session (SessionToken);
	)RAW";

	string_t FindUser = LR"RAW(
		SELECT * FROM User 
		WHERE Username = @Username
	)RAW";

	string_t CreateUser = LR"RAW(
		INSERT INTO User (Username,PasswordHash,HashMethod)
		VALUES(@Username,@PasswordHash,@HashMethod);
	)RAW";

	string_t FindSession = LR"RAW(
		SELECT * FROM Session 
		WHERE SessionToken = @SessionToken
	)RAW";

	string_t VerifySessionAndCSRF = LR"RAW(
		SELECT * FROM Session 
		WHERE SessionToken = @SessionToken
		AND CSRFToken = @CSRFToken
	)RAW";

	string_t CreateSession = LR"RAW(
		INSERT INTO Session (SessionToken,CSRFToken,Username,LastUsed)
		VALUES(@SessionToken,@CSRFToken,@Username,@LastUsed);
	)RAW";

	string_t DeleteSession = LR"RAW(
		DELETE FROM Session
		WHERE SessionToken = @SessionToken;
	)RAW";

	string_t GetUserCount = L"SELECT COUNT(*) FROM User";

#define CREATE_QUERY( X ) DB->CreateQuery( _T(#X), X )

	shared_ptr<SQLiteDatabase> InitializeDatabase( string_t Filename )
	{
		auto DB = make_shared<SQLiteDatabase>( Filename, Database::InitializationScript, true );

		CREATE_QUERY( FindUser );
		CREATE_QUERY( CreateUser );

		CREATE_QUERY( FindSession );
		CREATE_QUERY( VerifySessionAndCSRF );
		CREATE_QUERY( CreateSession );
		CREATE_QUERY( DeleteSession );

		CREATE_QUERY( GetUserCount );

		return DB;
	}
}