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

		CREATE TABLE IF NOT EXISTS Camera(
			CameraUID		INT									NOT NULL,
			CameraName		CHAR(64)							NOT NULL,
			CameraString	TEXT								NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS CameraIndex ON Camera (CameraUID);


		CREATE TABLE IF NOT EXISTS Clip(
			Timestamp		DATETIME,
			Camera			INT,
			MotionTimestamp	DATETIME,
			ActiveDuration	INT,
			Duration		INT,
			RecordMode		INT,
			MaxMotion		FLOAT,
			Description		TEXT
		);

		CREATE UNIQUE INDEX IF NOT EXISTS ClipIndex ON Clip (Timestamp,Camera);

		CREATE TABLE IF NOT EXISTS Tag(
			TagUID			INT									NOT NULL,
			Name			CHAR(64)							NOT NULL,
			Description		TEXT								NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS TagIndex ON Tag (TagUID);


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

	string_t VerifySession = LR"RAW(
		SELECT * FROM Session 
		WHERE SessionToken = @SessionToken
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

	string_t GetCameras = LR"RAW(
		SELECT * FROM Camera 
		ORDER BY CameraUID
	)RAW";

	string_t CreateClip = LR"RAW(
		INSERT INTO Clip (Timestamp,Camera,MotionTimestamp,ActiveDuration,Duration,RecordMode,MaxMotion,Description)
		VALUES(@Timestamp,@Camera,@MotionTimestamp,@ActiveDuration,@Duration,@RecordMode,@MaxMotion,@Description);
	)RAW";

	string_t UpdateClip = LR"RAW(
		UPDATE Clip 
		SET
			MotionTimestamp = @MotionTimestamp,
			ActiveDuration = @ActiveDuration,
			Duration = @Duration,
			MaxMotion = @MaxMotion
		WHERE 
				Timestamp == @Timestamp 
			AND Camera == @Camera
		;
	)RAW";

#define CREATE_QUERY( X ) DB->CreateQuery( _T(#X), X )

	shared_ptr<SQLiteDatabase> InitializeDatabase( string_t Filename )
	{
		auto DB = make_shared<SQLiteDatabase>( Filename, Database::InitializationScript, true, 
			[]( const string& Message )
			{
				cout << Message << endl;
			}
		);

		CREATE_QUERY( FindUser );
		CREATE_QUERY( CreateUser );

		CREATE_QUERY( FindSession );
		CREATE_QUERY( VerifySession );
		CREATE_QUERY( VerifySessionAndCSRF );
		CREATE_QUERY( CreateSession );
		CREATE_QUERY( DeleteSession );

		CREATE_QUERY( GetUserCount );

		CREATE_QUERY( GetCameras );

		CREATE_QUERY( CreateClip );
		CREATE_QUERY( UpdateClip );

		return DB;
	}
}