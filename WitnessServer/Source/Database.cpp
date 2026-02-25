#include "Common.h"
#include "SQLite.h"

namespace Database
{
	std::string InitializationScript = R"RAW(
		CREATE TABLE IF NOT EXISTS Setting(
			Name			TEXT PRIMARY KEY,
			Value			TEXT
		);

		CREATE TABLE IF NOT EXISTS User(
			UserUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
			Username		CHAR(64)							NOT NULL,
			DisplayName		CHAR(128)							NOT NULL,
			PasswordHash	CHAR(128)							NOT NULL,
			HashMethod		INTEGER								NOT NULL,
			Enabled			INTEGER								NOT NULL,		
			Admin			INTEGER								NOT NULL,
			MustChangePassword INTEGER							NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS UserIndex ON User (Username);

		CREATE TABLE IF NOT EXISTS Session(
			SessionToken	CHAR(64)							NOT NULL,
			CSRFToken		CHAR(64)							NOT NULL,
			UserUID			INTEGER								NOT NULL,
			LastUsed		DATETIME							NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS SessionIndex ON Session (SessionToken);

		CREATE TABLE IF NOT EXISTS Camera(
			CameraUID		INTEGER PRIMARY KEY	AUTOINCREMENT,
			CameraName		CHAR(64)							NOT NULL,
			CameraString	TEXT								NOT NULL,
			CameraStringSub	TEXT								NOT NULL,
			Description		TEXT,
			Enabled			INTEGER NOT NULL,
			SkipFrames		INTEGER NOT NULL,
			MDFrameHeight	INTEGER NOT NULL,
			MDThreshold		NUMERIC NOT NULL,
			MotionFilter	TEXT,
			BlackoutMaskPath TEXT,
			FocusMaskPath TEXT
		);

		CREATE TABLE IF NOT EXISTS CameraGroup (
			GroupUID		INTEGER PRIMARY KEY AUTOINCREMENT,
			DisplayName		TEXT UNIQUE,
			Description		TEXT
		);

		CREATE TABLE IF NOT EXISTS CameraGroupMapping (
			Camera		INTEGER									NOT NULL,
			`Group`		INTEGER									NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS CameraGroupMappingIndex ON CameraGroupMapping (Camera,`Group`);

		CREATE TABLE IF NOT EXISTS UserGroupMapping (
			UserUID		INTEGER										NOT NULL,
			`Group`		INTEGER										NOT NULL
		);

		CREATE UNIQUE INDEX IF NOT EXISTS UserGroupMappingIndex ON UserGroupMapping (UserUID,`Group`);

		CREATE TABLE IF NOT EXISTS Clip(
			ClipUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
			Timestamp		DATETIME,
			Camera			INT,
			MotionTimestamp	DATETIME,
			ActiveDuration	INT,
			Duration		INT,
			RecordMode		INT,
			MaxMotion		FLOAT,
			Description		TEXT,
			Save			INT,
			Tags			TEXT
		);

		CREATE UNIQUE INDEX IF NOT EXISTS ClipIndex ON Clip (Timestamp,Camera);

		CREATE TABLE IF NOT EXISTS Tag(
			TagUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
			Name			CHAR(64)							NOT NULL,
			Description		TEXT								NOT NULL
		);

		CREATE TABLE IF NOT EXISTS Action(
			ActionUID		INTEGER PRIMARY KEY	AUTOINCREMENT,
			Name			CHAR(64)							NOT NULL,
			Command			CHAR(64)							NOT NULL,
			Param1			TEXT								NOT NULL,
			Param2			TEXT								NOT NULL,
			Param3			TEXT								NOT NULL
		);

		CREATE TABLE IF NOT EXISTS CameraAction(
			CameraActionUID INTEGER PRIMARY KEY	AUTOINCREMENT,
			ActionUID		INTEGER,
			CameraUID		INTEGER,
			MDThreshold		FLOAT
		);

		CREATE UNIQUE INDEX IF NOT EXISTS CameraActionIndex ON CameraAction (ActionUID,CameraUID);

	)RAW";

	StringT GetSetting = LR"RAW(
		SELECT Value FROM Setting
		WHERE Name = @Name
	)RAW";

	StringT GetAllSettings = LR"RAW(
		SELECT * FROM Setting
	)RAW";

	StringT SetSetting = LR"RAW(
		INSERT OR REPLACE INTO Setting(Name, Value) VALUES(@Name, @Value)
	)RAW";

	StringT FindActions = LR"RAW(
		SELECT ActionUID FROM CameraAction
		WHERE CameraUID = @CameraUID 
		AND MDThreshold <= @MDThreshold
	)RAW";

	StringT GetAction = LR"RAW(
		SELECT * FROM Action
		WHERE ActionUID = @ActionUID
	)RAW";

	StringT FindUser = LR"RAW(
		SELECT UserUID, Username, PasswordHash, HashMethod FROM User 
		WHERE Username = @Username
	)RAW";

	StringT FindUserForAuth = LR"RAW(
		SELECT UserUID, Username, DisplayName, Enabled, Admin FROM User 
		WHERE UserUID = @UserUID
	)RAW";

	StringT FindUsers = LR"RAW(
		SELECT UserUID, Username, DisplayName, Enabled, Admin FROM User 
	)RAW";

	StringT CreateUser = LR"RAW(
		INSERT INTO User (Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin)
		VALUES(@Username,@DisplayName,@PasswordHash,@HashMethod,@Enabled,@Admin);
	)RAW";

	StringT DeleteUser = LR"RAW(
		DELETE FROM UserGroupMapping
			WHERE UserUID = @UserUID;
		DELETE FROM Session
			WHERE UserUID = @UserUID;
		DELETE FROM User
			WHERE UserUID = @UserUID;
	)RAW";

	StringT SetUserEnabledState = LR"RAW(
		UPDATE User 
		SET
			Enabled = @Enabled
		WHERE 
			Username == @Username
		;
	)RAW";

	StringT SetUserAdminState = LR"RAW(
		UPDATE User 
		SET
			Admin = @Admin
		WHERE 
			Username == @Username
		;
	)RAW";

	StringT SetUserDisplayName = LR"RAW(
		UPDATE User 
		SET
			DisplayName = @DisplayName
		WHERE 
			Username == @Username
		;
	)RAW";

	StringT FindSession = LR"RAW(
		SELECT * FROM Session 
		WHERE SessionToken = @SessionToken
	)RAW";

	StringT VerifySessionAndCSRF = LR"RAW(
		SELECT UserUID FROM Session 
		WHERE 
			SessionToken = @SessionToken
		AND	CSRFToken = @CSRFToken
	)RAW";

	StringT VerifySession = LR"RAW(
		SELECT UserUID FROM Session 
		WHERE 
			SessionToken = @SessionToken
	)RAW";

	StringT CreateSession = LR"RAW(
		INSERT INTO Session (SessionToken,CSRFToken,UserUID,LastUsed)
		VALUES(@SessionToken,@CSRFToken,@UserUID,@LastUsed);
	)RAW";

	StringT DeleteSession = LR"RAW(
		DELETE FROM Session
		WHERE SessionToken = @SessionToken;
	)RAW";

	StringT GetUserCount = L"SELECT COUNT(*) FROM User";

	StringT CreateCamera = LR"RAW(
		INSERT INTO Camera (CameraName, CameraString, CameraStringSub, Description, Enabled, SkipFrames, MDFrameHeight, MDThreshold, MotionFilter, BlackoutMaskPath, FocusMaskPath)
		VALUES(@CameraName,@CameraString,@CameraStringSub,@Description,1,1,400,0.0001,NULL,NULL,NULL);
	)RAW";

	
	StringT DeleteCamera = LR"RAW(
		DELETE FROM Camera 
		WHERE 
				CameraUID == @CameraId 
		;
	)RAW";

	StringT GetCameras = LR"RAW(
		SELECT * FROM Camera 
		ORDER BY CameraUID
	)RAW";

	StringT GetCamera = LR"RAW(
		SELECT * FROM Camera 
		WHERE CameraUID = @CameraId
		ORDER BY CameraUID
	)RAW";

	StringT GetCamerasForUser = LR"RAW(
		SELECT * FROM Camera C
		INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
		INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE UGM.UserUID = @User
	)RAW";

	StringT GetCamerasDetailsForUser = LR"RAW(
		SELECT * FROM Camera C
		INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
		INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE UGM.UserUID = @User
		AND C.CameraUID = @Camera
	)RAW";

	StringT CreateClip = LR"RAW(
		INSERT INTO Clip (Timestamp,Camera,MotionTimestamp,ActiveDuration,Duration,RecordMode,MaxMotion,Description,Save,Tags)
		VALUES(@Timestamp,@Camera,@MotionTimestamp,@ActiveDuration,@Duration,@RecordMode,@MaxMotion,@Description,@Save,@Tags);
	)RAW";

	StringT SelectClipID = LR"RAW(
		SELECT * FROM Clip
		WHERE Clip.ClipUID = @ClipUID
	)RAW";

	StringT UpdateClip = LR"RAW(
		UPDATE Clip 
		SET
			MotionTimestamp = @MotionTimestamp,
			ActiveDuration = @ActiveDuration,
			Duration = @Duration,
			MaxMotion = @MaxMotion,
			Tags = @Tags
		WHERE 
				Timestamp == @Timestamp 
			AND Camera == @Camera
		;
	)RAW";

	StringT SetClipSaveState = LR"RAW(
		UPDATE Clip 
		SET
			Save = @Save
		WHERE 
				ClipUID == @ClipUID
		;
	)RAW";

	StringT FindClipByUID = LR"RAW(
		SELECT * FROM Clip 
		WHERE 
			ClipUID == @ClipUID 
		;
	)RAW";
	
	StringT DeleteClip = LR"RAW(
		DELETE FROM Clip 
		WHERE 
				ClipUID == @ClipUID 
		;
	)RAW";

	StringT CountClipsWithinRange = LR"RAW(
		SELECT COUNT(Timestamp) FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		;
	)RAW";

	StringT CountClipsWithinRangeAll = LR"RAW(
		SELECT COUNT(Timestamp) FROM Clip
			INNER JOIN Camera C ON C.CameraUID = Clip.Camera
			INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
			INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE
				Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
			AND UGM.UserUID == @UserUID
	)RAW";

	StringT SelectClipsWithinRange = LR"RAW(
		SELECT * FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		ORDER BY Timestamp DESC
		LIMIT @MaxCount OFFSET @PageOffset
		;
	)RAW";

	StringT SelectClipsWithinRangeAll = LR"RAW(
		SELECT DISTINCT Clip.* FROM Clip
			INNER JOIN Camera C ON C.CameraUID = Clip.Camera
			INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
			INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE
				Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
			AND UGM.UserUID == @UserUID
		ORDER BY Timestamp DESC
		LIMIT @MaxCount OFFSET @PageOffset
		;
	)RAW";

	StringT SelectClip = LR"RAW(
		SELECT * FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp == @Timestamp
		;
	)RAW";

	StringT SelectClipsToDelete = LR"RAW(
		SELECT * FROM Clip
		WHERE
			Timestamp < @Timestamp
			AND (Save == 0 OR Save IS NULL)
		LIMIT 500;
	)RAW";

	StringT SelectAllGroups = LR"RAW(
		SELECT * FROM CameraGroup
		;
	)RAW";

	StringT SelectGroupsForCamera = LR"RAW(
		SELECT * FROM CameraGroupMapping
		WHERE Camera == @Camera
		;
	)RAW";

	StringT CreateGroup = LR"RAW(
		INSERT INTO CameraGroup (DisplayName,Description)
		VALUES(@DisplayName,@Description);
	)RAW";

	StringT UpdateGroup = LR"RAW(
		UPDATE CameraGroup 
		SET
			DisplayName = @DisplayName,
			Description = @Description
		WHERE
			GroupUID = @GroupUID
		;
	)RAW";

	StringT DeleteGroup = LR"RAW(
		DELETE FROM CameraGroupMapping
			WHERE `Group` = @Group;
		DELETE FROM UserGroupMapping
			WHERE `Group` = @Group;
		DELETE FROM CameraGroup
			WHERE GroupUID = @GroupUID;
	)RAW";

	StringT SelectGroupsForUser = LR"RAW(
		SELECT * FROM UserGroupMapping
		WHERE UserUID == @User
		;
	)RAW";

	StringT CreateUserGroupMapping = LR"RAW(
		INSERT INTO UserGroupMapping (UserUID,`Group`)
		VALUES(@UserUID,@Group);
	)RAW";

	StringT DeleteUserGroupMapping = LR"RAW(
		DELETE FROM UserGroupMapping
		WHERE UserUID = @UserUID
		AND `Group` = @Group;
	)RAW";

	StringT CreateCameraGroupMapping = LR"RAW(
		INSERT INTO CameraGroupMapping (Camera,`Group`)
		VALUES(@Camera,@Group);
	)RAW";

	StringT DeleteCameraGroupMapping = LR"RAW(
		DELETE FROM CameraGroupMapping
		WHERE Camera = @Camera
		AND `Group` = @Group;
	)RAW";

#define CREATE_QUERY( X ) DB->CreateQuery( _T(#X), X )

	std::shared_ptr<SQLiteDatabase> InitializeDatabase( StringT Filename )
	{
		auto DB = std::make_shared<SQLiteDatabase>( Filename, Database::InitializationScript, true,
			[]( const std::string& Message )
			{
				std::cout << Message << std::endl;
			}
		);

		CREATE_QUERY( GetSetting );
		CREATE_QUERY( GetAllSettings );
		CREATE_QUERY( SetSetting );

		CREATE_QUERY( FindUser );
		CREATE_QUERY( FindUserForAuth );
		CREATE_QUERY( FindUsers );
		CREATE_QUERY( CreateUser );
		CREATE_QUERY( DeleteUser );
		CREATE_QUERY( SetUserEnabledState );
		CREATE_QUERY( SetUserAdminState );
		CREATE_QUERY( SetUserDisplayName );

		CREATE_QUERY( FindSession );
		CREATE_QUERY( VerifySession );
		CREATE_QUERY( VerifySessionAndCSRF );
		CREATE_QUERY( CreateSession );
		CREATE_QUERY( DeleteSession );

		CREATE_QUERY( GetUserCount );

		CREATE_QUERY( CreateCamera );
		CREATE_QUERY( GetCameras );
		CREATE_QUERY( GetCamera );
		CREATE_QUERY( GetCamerasForUser );
		CREATE_QUERY( GetCamerasDetailsForUser );
		CREATE_QUERY( DeleteCamera );

		CREATE_QUERY( CreateClip );
		CREATE_QUERY( SelectClipID );
		CREATE_QUERY( UpdateClip );
		CREATE_QUERY( DeleteClip );
		CREATE_QUERY( SelectClip );
		CREATE_QUERY( SelectClipsToDelete );
		CREATE_QUERY( SetClipSaveState );
		CREATE_QUERY( FindClipByUID );
		CREATE_QUERY( CountClipsWithinRange );
		CREATE_QUERY( CountClipsWithinRangeAll );
		CREATE_QUERY( SelectClipsWithinRange );
		CREATE_QUERY( SelectClipsWithinRangeAll );

		CREATE_QUERY( SelectAllGroups );
		CREATE_QUERY( SelectGroupsForCamera );
		CREATE_QUERY( CreateGroup );
		CREATE_QUERY( UpdateGroup );
		CREATE_QUERY( DeleteGroup );
		CREATE_QUERY( CreateUserGroupMapping );
		CREATE_QUERY( DeleteUserGroupMapping );
		CREATE_QUERY( CreateCameraGroupMapping );
		CREATE_QUERY( DeleteCameraGroupMapping );

		CREATE_QUERY( SelectGroupsForUser );

		CREATE_QUERY( FindActions );
		CREATE_QUERY( GetAction );

		return DB;
	}
}