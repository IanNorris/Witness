#include "Common.h"
#include "SQLite.h"

namespace Database
{
	string InitializationScript = R"RAW(
		CREATE TABLE IF NOT EXISTS User(
			UserUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
			Username		CHAR(64)							NOT NULL,
			DisplayName		CHAR(128)							NOT NULL,
			PasswordHash	CHAR(128)							NOT NULL,
			HashMethod		INTEGER								NOT NULL,
			Enabled			INTEGER								NOT NULL,		
			Admin			INTEGER								NOT NULL
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
			CameraUID		INTEGER PRIMARY KEY	AUTOINCREMENT,
			CameraName		CHAR(64)							NOT NULL,
			CameraString	TEXT								NOT NULL,
			Description		TEXT
		);

		CREATE TABLE IF NOT EXISTS CameraGroup (
			GroupUID		INTEGER PRIMARY KEY AUTOINCREMENT,
			DisplayName		TEXT UNIQUE,
			Description		TEXT
		);

		CREATE TABLE IF NOT EXISTS CameraGroupMapping (
			Camera	INTEGER										NOT NULL,
			`Group`	INTEGER										NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS CameraGroupMappingIndex ON CameraGroupMapping (Camera,`Group`);

		CREATE TABLE IF NOT EXISTS Clip(
			ClipUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
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
			TagUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
			Name			CHAR(64)							NOT NULL,
			Description		TEXT								NOT NULL
		);

	)RAW";

	string_t FindUser = LR"RAW(
		SELECT Username, PasswordHash, HashMethod FROM User 
		WHERE Username = @Username
	)RAW";

	string_t FindUserForAuth = LR"RAW(
		SELECT DisplayName, Enabled, Admin FROM User 
		WHERE Username = @Username
	)RAW";

	string_t FindUsers = LR"RAW(
		SELECT Username, DisplayName, Enabled, Admin FROM User 
	)RAW";

	string_t CreateUser = LR"RAW(
		INSERT INTO User (Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin)
		VALUES(@Username,@DisplayName,@PasswordHash,@HashMethod,@Enabled,@Admin);
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

	string_t CountClipsWithinRange = LR"RAW(
		SELECT COUNT(Timestamp) FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		;
	)RAW";

	string_t SelectClipsWithinRange = LR"RAW(
		SELECT * FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		ORDER BY Timestamp DESC
		LIMIT @MaxCount OFFSET @PageOffset
		;
	)RAW";

	string_t SelectClip = LR"RAW(
		SELECT * FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp == @Timestamp
		;
	)RAW";

	string_t SelectAllGroups = LR"RAW(
		SELECT * FROM CameraGroup
		;
	)RAW";

	string_t SelectGroupsForCamera = LR"RAW(
		SELECT * FROM CameraGroupMapping
		WHERE Camera == @Camera
		;
	)RAW";

	string_t CreateGroup = LR"RAW(
		INSERT INTO CameraGroup (DisplayName,Description)
		VALUES(@DisplayName,@Description);
	)RAW";

	string_t UpdateGroup = LR"RAW(
		UPDATE CameraGroup 
		SET
			DisplayName = @DisplayName,
			Description = @Description
		WHERE
			GroupUID = @GroupUID
		;
	)RAW";

	string_t DeleteGroup = LR"RAW(
		DELETE FROM CameraGroup
		WHERE GroupUID = @GroupUID;
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
		CREATE_QUERY( FindUserForAuth );
		CREATE_QUERY( FindUsers );
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
		CREATE_QUERY( SelectClip );
		CREATE_QUERY( CountClipsWithinRange );
		CREATE_QUERY( SelectClipsWithinRange );

		CREATE_QUERY( SelectAllGroups );
		CREATE_QUERY( SelectGroupsForCamera );
		CREATE_QUERY( CreateGroup );
		CREATE_QUERY( UpdateGroup );
		CREATE_QUERY( DeleteGroup );

		return DB;
	}
}