#include "Common.h"
#include "SQLite.h"

#include <Log.h>

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
			MustChangePassword INTEGER							NOT NULL DEFAULT 0
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
			Tags			TEXT,
			DetectionVersion INT DEFAULT 0
		);

		CREATE UNIQUE INDEX IF NOT EXISTS ClipIndex ON Clip (Timestamp,Camera);

		CREATE TABLE IF NOT EXISTS Tag(
			TagUID			INTEGER PRIMARY KEY	AUTOINCREMENT,
			Name			TEXT UNIQUE				NOT NULL,
			Display			TEXT					NOT NULL,
			Icon			TEXT					DEFAULT '',
			SortOrder		INTEGER					DEFAULT 0,
			Hidden			INTEGER					DEFAULT 0
		);

		CREATE TABLE IF NOT EXISTS ClipTag(
			ClipUID			INTEGER					NOT NULL,
			TagUID			INTEGER					NOT NULL,
			PRIMARY KEY (ClipUID, TagUID),
			FOREIGN KEY (ClipUID) REFERENCES Clip(ClipUID) ON DELETE CASCADE,
			FOREIGN KEY (TagUID) REFERENCES Tag(TagUID)
		);

		CREATE INDEX IF NOT EXISTS ClipTagByTag ON ClipTag (TagUID);

		CREATE TABLE IF NOT EXISTS CameraTagExclusion(
			CameraID		INTEGER					NOT NULL,
			TagUID			INTEGER					NOT NULL,
			PRIMARY KEY (CameraID, TagUID),
			FOREIGN KEY (TagUID) REFERENCES Tag(TagUID)
		);

		CREATE TABLE IF NOT EXISTS ContinuousSegment(
			SegmentUID		INTEGER PRIMARY KEY AUTOINCREMENT,
			CameraUID		INTEGER					NOT NULL,
			StartTimestamp	INTEGER					NOT NULL,
			EndTimestamp	INTEGER					NOT NULL,
			Duration		INTEGER					NOT NULL,
			FilePath		TEXT					NOT NULL
		);

		CREATE INDEX IF NOT EXISTS ContinuousSegmentByCameraTime ON ContinuousSegment (CameraUID, StartTimestamp);

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
			MDThreshold		FLOAT,
			DetectionClass	TEXT DEFAULT ''
		);

		CREATE UNIQUE INDEX IF NOT EXISTS CameraActionIndex ON CameraAction (ActionUID,CameraUID,DetectionClass);

		CREATE TABLE IF NOT EXISTS DetectionFrame(
			FrameUID		INTEGER PRIMARY KEY AUTOINCREMENT,
			CameraID		INTEGER					NOT NULL,
			Timestamp		REAL					NOT NULL,
			FrameWidth		INTEGER,
			FrameHeight		INTEGER,
			FramePath		TEXT
		);

		CREATE INDEX IF NOT EXISTS idx_detframe_camera_time ON DetectionFrame(CameraID, Timestamp);

		CREATE TABLE IF NOT EXISTS DetectionBox(
			BoxUID			INTEGER PRIMARY KEY AUTOINCREMENT,
			FrameUID		INTEGER					NOT NULL,
			TrackingID		INTEGER					DEFAULT 0,
			ClassID			INTEGER,
			ClassName		TEXT,
			Confidence		REAL,
			X				REAL,
			Y				REAL,
			W				REAL,
			H				REAL,
			IsBaseline		INTEGER					DEFAULT 0,
			CropPath		TEXT,
			FOREIGN KEY(FrameUID) REFERENCES DetectionFrame(FrameUID) ON DELETE CASCADE
		);

		CREATE TABLE IF NOT EXISTS FaceCrop(
			CropUID			INTEGER PRIMARY KEY AUTOINCREMENT,
			CameraID		INTEGER					NOT NULL,
			Timestamp		REAL					NOT NULL,
			FrameUID		INTEGER,
			TrackingID		INTEGER					DEFAULT 0,
			FilePath		TEXT					NOT NULL,
			Confidence		REAL,
			Landmark0X		REAL, Landmark0Y		REAL,
			Landmark1X		REAL, Landmark1Y		REAL,
			Landmark2X		REAL, Landmark2Y		REAL,
			Landmark3X		REAL, Landmark3Y		REAL,
			Landmark4X		REAL, Landmark4Y		REAL,
			FOREIGN KEY(FrameUID) REFERENCES DetectionFrame(FrameUID) ON DELETE CASCADE
		);

		CREATE INDEX IF NOT EXISTS idx_facecrop_camera_time ON FaceCrop(CameraID, Timestamp);
		CREATE INDEX IF NOT EXISTS idx_facecrop_frame_track ON FaceCrop(FrameUID, TrackingID);

		CREATE TABLE IF NOT EXISTS KnownFace(
			KnownFaceUID	INTEGER PRIMARY KEY AUTOINCREMENT,
			Name			TEXT					NOT NULL,
			Notes			TEXT					DEFAULT '',
			CreatedAt		REAL					NOT NULL,
			UpdatedAt		REAL					NOT NULL
		);

		CREATE TABLE IF NOT EXISTS FaceEmbedding(
			EmbeddingUID	INTEGER PRIMARY KEY AUTOINCREMENT,
			FaceCropUID		INTEGER					NOT NULL,
			KnownFaceUID	INTEGER,
			Embedding		BLOB					NOT NULL,
			Dimension		INTEGER					NOT NULL,
			MatchConfidence	REAL,
			Verified		INTEGER					DEFAULT 0,
			CreatedAt		REAL					NOT NULL,
			FOREIGN KEY(FaceCropUID) REFERENCES FaceCrop(CropUID) ON DELETE CASCADE,
			FOREIGN KEY(KnownFaceUID) REFERENCES KnownFace(KnownFaceUID) ON DELETE SET NULL
		);

		CREATE INDEX IF NOT EXISTS idx_embedding_known ON FaceEmbedding(KnownFaceUID);
		CREATE INDEX IF NOT EXISTS idx_embedding_crop ON FaceEmbedding(FaceCropUID);

		CREATE TABLE IF NOT EXISTS Trail(
			TrailUID		INTEGER PRIMARY KEY AUTOINCREMENT,
			ClipUID			INTEGER			NOT NULL,
			CameraID		INTEGER			NOT NULL,
			ClassName		TEXT			NOT NULL,
			FaceName		TEXT,
			StartTime		REAL			NOT NULL,
			EndTime			REAL			NOT NULL,
			PointData		TEXT			NOT NULL,
			FOREIGN KEY(ClipUID) REFERENCES Clip(ClipUID) ON DELETE CASCADE
		);
		CREATE INDEX IF NOT EXISTS idx_trail_camera_time ON Trail(CameraID, StartTime, EndTime);

	)RAW";

	std::string GetSetting = R"RAW(
		SELECT Value FROM Setting
		WHERE Name = @Name
	)RAW";

	std::string GetAllSettings = R"RAW(
		SELECT * FROM Setting
	)RAW";

	std::string SetSetting = R"RAW(
		INSERT OR REPLACE INTO Setting(Name, Value) VALUES(@Name, @Value)
	)RAW";

	std::string FindActions = R"RAW(
		SELECT ca.ActionUID FROM CameraAction ca
		WHERE ca.CameraUID = @CameraUID
		AND (ca.DetectionClass = '' OR ca.DetectionClass IS NULL)
		AND ca.MDThreshold <= @MDThreshold
	)RAW";

	std::string GetAction = R"RAW(
		SELECT ActionUID, Name, Command, Param1, Param2, Param3, Priority, Cooldown FROM Action
		WHERE ActionUID = @ActionUID
	)RAW";

	std::string FindDetectionActions = R"RAW(
		SELECT ca.ActionUID, ca.MDThreshold FROM CameraAction ca
		WHERE ca.CameraUID = @CameraUID
		AND ca.DetectionClass = @DetectionClass
	)RAW";

	std::string SelectAllActions = R"RAW(
		SELECT ActionUID, Name, Command, Param1, Param2, Param3, Priority, Cooldown FROM Action
		ORDER BY Name ASC
	)RAW";

	std::string CreateAction = R"RAW(
		INSERT INTO Action(Name, Command, Param1, Param2, Param3, Priority, Cooldown)
		VALUES(@Name, @Command, @Param1, @Param2, @Param3, @Priority, @Cooldown)
	)RAW";

	std::string UpdateAction = R"RAW(
		UPDATE Action SET Name=@Name, Command=@Command, Param1=@Param1, Param2=@Param2, Param3=@Param3, Priority=@Priority, Cooldown=@Cooldown
		WHERE ActionUID=@ActionUID
	)RAW";

	std::string DeleteAction = R"RAW(
		DELETE FROM Action WHERE ActionUID=@ActionUID
	)RAW";

	std::string DeleteCameraActionsForAction = R"RAW(
		DELETE FROM CameraAction WHERE ActionUID=@ActionUID
	)RAW";

	std::string SelectCameraActionsForAction = R"RAW(
		SELECT ca.CameraActionUID, ca.ActionUID, ca.CameraUID, ca.MDThreshold, ca.DetectionClass
		FROM CameraAction ca
		WHERE ca.ActionUID = @ActionUID
		ORDER BY ca.CameraUID ASC
	)RAW";

	std::string SelectAllCameraActions = R"RAW(
		SELECT ca.CameraActionUID, ca.ActionUID, ca.CameraUID, ca.MDThreshold, ca.DetectionClass
		FROM CameraAction ca
		ORDER BY ca.ActionUID ASC, ca.CameraUID ASC
	)RAW";

	std::string CreateCameraAction = R"RAW(
		INSERT OR REPLACE INTO CameraAction(ActionUID, CameraUID, MDThreshold, DetectionClass)
		VALUES(@ActionUID, @CameraUID, @MDThreshold, @DetectionClass)
	)RAW";

	std::string DeleteCameraAction = R"RAW(
		DELETE FROM CameraAction WHERE CameraActionUID=@CameraActionUID
	)RAW";

	std::string SelectNotificationSounds = R"RAW(
		SELECT 1
	)RAW";

	std::string FindUser = R"RAW(
		SELECT UserUID, Username, PasswordHash, HashMethod FROM User 
		WHERE Username = @Username
	)RAW";

	std::string FindUserForAuth = R"RAW(
		SELECT UserUID, Username, DisplayName, Enabled, Admin FROM User 
		WHERE UserUID = @UserUID
	)RAW";

	std::string FindUsers = R"RAW(
		SELECT UserUID, Username, DisplayName, Enabled, Admin FROM User 
	)RAW";

	std::string CreateUser = R"RAW(
		INSERT INTO User (Username,DisplayName,PasswordHash,HashMethod,Enabled,Admin)
		VALUES(@Username,@DisplayName,@PasswordHash,@HashMethod,@Enabled,@Admin);
	)RAW";

	std::string DeleteUser = R"RAW(
		DELETE FROM UserGroupMapping
			WHERE UserUID = @UserUID;
		DELETE FROM Session
			WHERE UserUID = @UserUID;
		DELETE FROM User
			WHERE UserUID = @UserUID;
	)RAW";

	std::string SetUserEnabledState = R"RAW(
		UPDATE User 
		SET
			Enabled = @Enabled
		WHERE 
			Username == @Username
		;
	)RAW";

	std::string SetUserAdminState = R"RAW(
		UPDATE User 
		SET
			Admin = @Admin
		WHERE 
			Username == @Username
		;
	)RAW";

	std::string SetUserDisplayName = R"RAW(
		UPDATE User 
		SET
			DisplayName = @DisplayName
		WHERE 
			Username == @Username
		;
	)RAW";

	std::string SetUserPassword = R"RAW(
		UPDATE User 
		SET
			PasswordHash = @PasswordHash,
			HashMethod = @HashMethod
		WHERE 
			Username == @Username
		;
	)RAW";

	std::string FindSession = R"RAW(
		SELECT * FROM Session 
		WHERE SessionToken = @SessionToken
	)RAW";

	std::string VerifySessionAndCSRF = R"RAW(
		SELECT UserUID FROM Session 
		WHERE 
			SessionToken = @SessionToken
		AND	CSRFToken = @CSRFToken
	)RAW";

	std::string VerifySession = R"RAW(
		SELECT UserUID FROM Session 
		WHERE 
			SessionToken = @SessionToken
	)RAW";

	std::string CreateSession = R"RAW(
		INSERT INTO Session (SessionToken,CSRFToken,UserUID,LastUsed)
		VALUES(@SessionToken,@CSRFToken,@UserUID,@LastUsed);
	)RAW";

	std::string DeleteSession = R"RAW(
		DELETE FROM Session
		WHERE SessionToken = @SessionToken;
	)RAW";

	std::string DeleteUserSessions = R"RAW(
		DELETE FROM Session
		WHERE UserUID = (SELECT UserUID FROM User WHERE Username = @Username);
	)RAW";

	std::string GetUserCount = "SELECT COUNT(*) FROM User";

	std::string CreateCamera = R"RAW(
		INSERT INTO Camera (CameraName, CameraString, CameraStringSub, Description, Enabled, SkipFrames, MDFrameHeight, MDThreshold, MotionFilter, BlackoutMaskPath, FocusMaskPath)
		VALUES(@CameraName,@CameraString,@CameraStringSub,@Description,1,1,400,0.0001,NULL,NULL,NULL);
	)RAW";

	
	std::string UpdateCamera = R"RAW(
		UPDATE Camera SET
			CameraName = @CameraName,
			CameraString = @CameraString,
			CameraStringSub = @CameraStringSub,
			Description = @Description,
			Enabled = @Enabled,
			SkipFrames = @SkipFrames,
			MDFrameHeight = @MDFrameHeight,
			MDThreshold = @MDThreshold,
			MotionFilter = @MotionFilter,
			BlackoutMaskPath = @BlackoutMaskPath,
			FocusMaskPath = @FocusMaskPath,
			ContinuousRecording = @ContinuousRecording,
			LowLatencyHLS = @LowLatencyHLS
		WHERE CameraUID = @CameraId;
	)RAW";

	std::string DeleteCamera = R"RAW(
		DELETE FROM Camera 
		WHERE 
				CameraUID == @CameraId 
		;
	)RAW";

	std::string GetCameras = R"RAW(
		SELECT * FROM Camera 
		ORDER BY CameraUID
	)RAW";

	std::string GetCamera = R"RAW(
		SELECT * FROM Camera 
		WHERE CameraUID = @CameraId
		ORDER BY CameraUID
	)RAW";

	std::string GetCamerasForUser = R"RAW(
		SELECT DISTINCT C.* FROM Camera C
		INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
		INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE UGM.UserUID = @User
	)RAW";

	std::string GetCamerasDetailsForUser = R"RAW(
		SELECT * FROM Camera C
		INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
		INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE UGM.UserUID = @User
		AND C.CameraUID = @Camera
	)RAW";

	std::string CreateClip = R"RAW(
		INSERT INTO Clip (Timestamp,Camera,MotionTimestamp,ActiveDuration,Duration,RecordMode,MaxMotion,Description,Save,Tags)
		VALUES(@Timestamp,@Camera,@MotionTimestamp,@ActiveDuration,@Duration,@RecordMode,@MaxMotion,@Description,@Save,@Tags);
	)RAW";

	std::string SelectClipID = R"RAW(
		SELECT * FROM Clip
		WHERE Clip.ClipUID = @ClipUID
	)RAW";

	std::string UpdateClip = R"RAW(
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

	std::string SetClipSaveState = R"RAW(
		UPDATE Clip 
		SET
			Save = @Save
		WHERE 
				ClipUID == @ClipUID
		;
	)RAW";

	std::string FindClipByUID = R"RAW(
		SELECT * FROM Clip 
		WHERE 
			ClipUID == @ClipUID 
		;
	)RAW";
	
	std::string DeleteClip = R"RAW(
		DELETE FROM ClipTag WHERE ClipUID == @ClipUID;
		DELETE FROM Clip 
		WHERE 
				ClipUID == @ClipUID 
		;
	)RAW";

	std::string CountClipsWithinRange = R"RAW(
		SELECT COUNT(Timestamp) FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		;
	)RAW";

	std::string CountClipsWithinRangeAll = R"RAW(
		SELECT COUNT(Timestamp) FROM Clip
			INNER JOIN Camera C ON C.CameraUID = Clip.Camera
			INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
			INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE
				Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
			AND UGM.UserUID == @UserUID
	)RAW";

	std::string SelectClipsWithinRange = R"RAW(
		SELECT * FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		ORDER BY Timestamp DESC
		LIMIT @MaxCount OFFSET @PageOffset
		;
	)RAW";

	std::string SelectClipsWithinRangeAll = R"RAW(
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

	std::string SelectClip = R"RAW(
		SELECT * FROM Clip
		WHERE
				Camera == @CameraID
			AND	Timestamp == @Timestamp
		;
	)RAW";

	std::string SelectClipsToDelete = R"RAW(
		SELECT * FROM Clip
		WHERE
			Timestamp < @Timestamp
			AND (Save == 0 OR Save IS NULL)
		LIMIT 50;
	)RAW";

	std::string SelectClipForReprocess = R"RAW(
		SELECT * FROM Clip
		WHERE DetectionVersion < @DetectionVersion OR DetectionVersion IS NULL
		ORDER BY DetectionVersion ASC, Timestamp DESC
		LIMIT 1;
	)RAW";

	std::string UpdateClipDetection = R"RAW(
		UPDATE Clip
		SET Tags = @Tags, DetectionVersion = @DetectionVersion, Lighting = @Lighting
		WHERE ClipUID = @ClipUID;
	)RAW";

	std::string ResetClipDetection = R"RAW(
		DELETE FROM ClipTag WHERE ClipUID = @ClipUID;
		UPDATE Clip
		SET DetectionVersion = -1, Tags = ''
		WHERE ClipUID = @ClipUID;
	)RAW";

	std::string ResetClipDetectionBulk = R"RAW(
		UPDATE Clip SET DetectionVersion = 0
		WHERE (@CameraID = -1 OR Camera = @CameraID)
		  AND Timestamp >= @TimestampFrom
		  AND Timestamp <= @TimestampTo;
	)RAW";

	std::string CountClipsToReprocess = R"RAW(
		SELECT COUNT(*) FROM Clip
		WHERE DetectionVersion < @DetectionVersion OR DetectionVersion IS NULL;
	)RAW";

	std::string SelectReprocessQueue = R"RAW(
		SELECT c.ClipUID, c.Timestamp, c.Camera, c.DetectionVersion, c.RecordMode, c.Tags, c.Duration
		FROM Clip c
		WHERE c.DetectionVersion < @DetectionVersion OR c.DetectionVersion IS NULL
		ORDER BY c.DetectionVersion ASC, c.Timestamp DESC
		LIMIT 100;
	)RAW";

	std::string SelectClipsNeedingLighting = R"RAW(
		SELECT ClipUID, Timestamp, Camera, RecordMode FROM Clip
		WHERE Lighting = 0 OR Lighting IS NULL
		ORDER BY Timestamp DESC
		LIMIT 200;
	)RAW";

	std::string UpdateClipLighting = R"RAW(
		UPDATE Clip SET Lighting = @Lighting WHERE ClipUID = @ClipUID;
	)RAW";

	std::string SelectAllGroups = R"RAW(
		SELECT * FROM CameraGroup
		;
	)RAW";

	std::string SelectGroupsForCamera = R"RAW(
		SELECT * FROM CameraGroupMapping
		WHERE Camera == @Camera
		;
	)RAW";

	std::string CreateGroup = R"RAW(
		INSERT INTO CameraGroup (DisplayName,Description)
		VALUES(@DisplayName,@Description);
	)RAW";

	std::string UpdateGroup = R"RAW(
		UPDATE CameraGroup 
		SET
			DisplayName = @DisplayName,
			Description = @Description
		WHERE
			GroupUID = @GroupUID
		;
	)RAW";

	std::string DeleteGroup = R"RAW(
		DELETE FROM CameraGroupMapping
			WHERE `Group` = @Group;
		DELETE FROM UserGroupMapping
			WHERE `Group` = @Group;
		DELETE FROM CameraGroup
			WHERE GroupUID = @GroupUID;
	)RAW";

	std::string SelectGroupsForUser = R"RAW(
		SELECT * FROM UserGroupMapping
		WHERE UserUID == @User
		;
	)RAW";

	// Tag queries
	std::string FindOrCreateTag = R"RAW(
		INSERT OR IGNORE INTO Tag (Name, Display, Icon, SortOrder) VALUES(@Name, @Display, @Icon, @SortOrder);
	)RAW";

	std::string SelectTagByName = R"RAW(
		SELECT TagUID FROM Tag WHERE Name = @Name;
	)RAW";

	std::string SelectAllTags = R"RAW(
		SELECT t.TagUID, t.Name, t.Display, t.Icon, t.SortOrder, t.Hidden,
			(SELECT COUNT(*) FROM ClipTag ct WHERE ct.TagUID = t.TagUID) AS ClipCount
		FROM Tag t
		ORDER BY t.SortOrder, t.Display;
	)RAW";

	std::string UpdateTag = R"RAW(
		UPDATE Tag SET Display = @Display, Icon = @Icon, Hidden = @Hidden
		WHERE TagUID = @TagUID;
	)RAW";

	std::string InsertClipTag = R"RAW(
		INSERT OR IGNORE INTO ClipTag (ClipUID, TagUID) VALUES(@ClipUID, @TagUID);
	)RAW";

	std::string DeleteClipTags = R"RAW(
		DELETE FROM ClipTag WHERE ClipUID = @ClipUID;
	)RAW";

	std::string SelectTagsForClip = R"RAW(
		SELECT t.TagUID, t.Name, t.Display, t.Icon
		FROM Tag t
		INNER JOIN ClipTag ct ON ct.TagUID = t.TagUID
		WHERE ct.ClipUID = @ClipUID;
	)RAW";

	std::string SelectCameraTagExclusions = R"RAW(
		SELECT TagUID FROM CameraTagExclusion WHERE CameraID = @CameraID;
	)RAW";

	std::string InsertCameraTagExclusion = R"RAW(
		INSERT OR IGNORE INTO CameraTagExclusion (CameraID, TagUID) VALUES(@CameraID, @TagUID);
	)RAW";

	std::string DeleteCameraTagExclusions = R"RAW(
		DELETE FROM CameraTagExclusion WHERE CameraID = @CameraID;
	)RAW";

	// Clip review
	std::string SetClipReviewed = R"RAW(
		UPDATE Clip SET Reviewed = @Reviewed WHERE ClipUID = @ClipUID;
	)RAW";

	std::string SetAllClipsReviewed = R"RAW(
		UPDATE Clip SET Reviewed = 1 WHERE Reviewed = 0;
	)RAW";

	std::string SelectRecentUnreviewed = R"RAW(
		SELECT DISTINCT Clip.* FROM Clip
			INNER JOIN Camera C ON C.CameraUID = Clip.Camera
			INNER JOIN CameraGroupMapping CGM ON CGM.Camera = C.CameraUID
			INNER JOIN UserGroupMapping UGM ON UGM.`Group` = CGM.`Group`
		WHERE Reviewed = 0
			AND UGM.UserUID == @UserUID
		ORDER BY Timestamp DESC
		LIMIT @MaxCount;
	)RAW";

	// Calendar (event counts per day)
	std::string SelectClipCountsByDay = R"RAW(
		SELECT date(Timestamp, 'unixepoch', 'localtime') AS day, COUNT(*) AS cnt
		FROM Clip
		WHERE Timestamp >= @TimestampFrom AND Timestamp < @TimestampTo
		GROUP BY day
		ORDER BY day;
	)RAW";

	// Timeline: all clips for a day with camera name
	std::string SelectClipsForTimeline = R"RAW(
		SELECT Clip.ClipUID, Clip.Timestamp, Clip.Duration, Clip.Camera, Camera.CameraName,
			Clip.RecordMode, Clip.Lighting, Clip.Save, Clip.Reviewed
		FROM Clip
		INNER JOIN Camera ON Camera.CameraUID = Clip.Camera
		WHERE Clip.Timestamp >= @TimestampFrom AND Clip.Timestamp < @TimestampTo
		ORDER BY Clip.Timestamp ASC;
	)RAW";

	// Tag migration: select clips with non-empty legacy tags
	std::string SelectClipsWithTags = R"RAW(
		SELECT ClipUID, Tags FROM Clip
		WHERE Tags IS NOT NULL AND Tags != '';
	)RAW";

	// Clear legacy Tags column after migration to ClipTag
	std::string ClearLegacyClipTags = R"RAW(
		UPDATE Clip SET Tags = NULL WHERE ClipUID = @ClipUID;
	)RAW";

	std::string CreateUserGroupMapping = R"RAW(
		INSERT INTO UserGroupMapping (UserUID,`Group`)
		VALUES(@UserUID,@Group);
	)RAW";

	std::string DeleteUserGroupMapping = R"RAW(
		DELETE FROM UserGroupMapping
		WHERE UserUID = @UserUID
		AND `Group` = @Group;
	)RAW";

	std::string CreateCameraGroupMapping = R"RAW(
		INSERT INTO CameraGroupMapping (Camera,`Group`)
		VALUES(@Camera,@Group);
	)RAW";

	std::string DeleteCameraGroupMapping = R"RAW(
		DELETE FROM CameraGroupMapping
		WHERE Camera = @Camera
		AND `Group` = @Group;
	)RAW";

	// Continuous recording segment queries
	std::string CreateContinuousSegment = R"RAW(
		INSERT INTO ContinuousSegment (CameraUID, StartTimestamp, EndTimestamp, Duration, FilePath, FileSize)
		VALUES(@CameraUID, @StartTimestamp, @EndTimestamp, @Duration, @FilePath, @FileSize);
	)RAW";

	std::string SelectContinuousSegments = R"RAW(
		SELECT * FROM ContinuousSegment
		WHERE CameraUID = @CameraUID
			AND StartTimestamp <= @TimestampTo
			AND EndTimestamp >= @TimestampFrom
		ORDER BY StartTimestamp ASC;
	)RAW";

	std::string SelectContinuousSegmentsToDelete = R"RAW(
		SELECT SegmentUID, FilePath FROM ContinuousSegment
		WHERE EndTimestamp < @Timestamp
		ORDER BY EndTimestamp ASC
		LIMIT 100;
	)RAW";

	std::string DeleteContinuousSegment = R"RAW(
		DELETE FROM ContinuousSegment WHERE SegmentUID = @SegmentUID;
	)RAW";

	std::string SelectContinuousTotalSize = R"RAW(
		SELECT COUNT(*), COALESCE(SUM(Duration), 0), COALESCE(SUM(FileSize), 0) FROM ContinuousSegment;
	)RAW";

	std::string SelectContinuousSizePerCamera = R"RAW(
		SELECT CameraUID, COUNT(*), COALESCE(SUM(FileSize), 0) FROM ContinuousSegment GROUP BY CameraUID;
	)RAW";

	std::string SelectContinuousSegmentsWithNoFileSize = R"RAW(
		SELECT SegmentUID, FilePath FROM ContinuousSegment WHERE FileSize = 0 LIMIT 200;
	)RAW";

	std::string UpdateContinuousSegmentFileSize = R"RAW(
		UPDATE ContinuousSegment SET FileSize = @FileSize WHERE SegmentUID = @SegmentUID;
	)RAW";

	std::string SelectContinuousSegmentByFilePath = R"RAW(
		SELECT SegmentUID FROM ContinuousSegment WHERE FilePath = @FilePath LIMIT 1;
	)RAW";

	std::string SelectOldestContinuousSegment = R"RAW(
		SELECT SegmentUID, FilePath FROM ContinuousSegment
		ORDER BY StartTimestamp ASC
		LIMIT 1;
	)RAW";

	std::string SelectContinuousCoverage = R"RAW(
		SELECT StartTimestamp, EndTimestamp FROM ContinuousSegment
		WHERE CameraUID = @CameraUID
			AND StartTimestamp <= @TimestampTo
			AND EndTimestamp >= @TimestampFrom
		ORDER BY StartTimestamp ASC;
	)RAW";

	std::string SelectContinuousSegmentAtTimestamp = R"RAW(
		SELECT SegmentUID, FilePath, StartTimestamp, EndTimestamp FROM ContinuousSegment
		WHERE CameraUID = @CameraUID
			AND StartTimestamp <= @Timestamp
			AND EndTimestamp >= @Timestamp
		ORDER BY StartTimestamp DESC
		LIMIT 1;
	)RAW";

	// Detection overlay queries
	std::string InsertDetectionFrame = R"RAW(
		INSERT INTO DetectionFrame (CameraID, Timestamp, FrameWidth, FrameHeight, FramePath)
		VALUES(@CameraID, @Timestamp, @FrameWidth, @FrameHeight, @FramePath);
	)RAW";

	std::string InsertDetectionBox = R"RAW(
		INSERT INTO DetectionBox (FrameUID, TrackingID, ClassID, ClassName, Confidence, X, Y, W, H, IsBaseline, CropPath)
		VALUES(@FrameUID, @TrackingID, @ClassID, @ClassName, @Confidence, @X, @Y, @W, @H, @IsBaseline, @CropPath);
	)RAW";

	std::string SelectDetectionFramesWithBoxes = R"RAW(
		SELECT f.FrameUID, f.Timestamp, f.FrameWidth, f.FrameHeight,
			   b.TrackingID, b.ClassID, b.ClassName, b.Confidence, b.X, b.Y, b.W, b.H, b.IsBaseline, b.CropPath,
			   kf.Name, f.FramePath
		FROM DetectionFrame f
		LEFT JOIN DetectionBox b ON f.FrameUID = b.FrameUID
		LEFT JOIN FaceCrop fc ON fc.FrameUID = b.FrameUID AND fc.TrackingID = b.TrackingID
		LEFT JOIN FaceEmbedding fe ON fe.FaceCropUID = fc.CropUID AND fe.KnownFaceUID IS NOT NULL
		LEFT JOIN KnownFace kf ON kf.KnownFaceUID = fe.KnownFaceUID
		WHERE f.CameraID = @CameraID
			AND f.Timestamp >= @TimestampFrom
			AND f.Timestamp <= @TimestampTo
		ORDER BY f.Timestamp ASC, b.BoxUID ASC;
	)RAW";

	std::string DeleteDetectionFramesBefore = R"RAW(
		DELETE FROM DetectionBox WHERE FrameUID IN (
			SELECT FrameUID FROM DetectionFrame WHERE CameraID = @CameraID AND Timestamp < @Timestamp
		);
		DELETE FROM DetectionFrame
		WHERE CameraID = @CameraID AND Timestamp < @Timestamp;
	)RAW";

	std::string DeleteAllDetectionFrames = R"RAW(
		DELETE FROM DetectionBox WHERE FrameUID IN (
			SELECT FrameUID FROM DetectionFrame WHERE CameraID = @CameraID
		);
		DELETE FROM DetectionFrame WHERE CameraID = @CameraID;
	)RAW";

	std::string DeleteDetectionFramesInRange = R"RAW(
		DELETE FROM DetectionBox WHERE FrameUID IN (
			SELECT FrameUID FROM DetectionFrame WHERE CameraID = @CameraID AND Timestamp >= @TimestampFrom AND Timestamp <= @TimestampTo
		);
		DELETE FROM DetectionFrame
		WHERE CameraID = @CameraID AND Timestamp >= @TimestampFrom AND Timestamp <= @TimestampTo;
	)RAW";

	std::string InsertTrail = R"RAW(
		INSERT INTO Trail (ClipUID, CameraID, ClassName, FaceName, StartTime, EndTime, PointData)
		VALUES (@ClipUID, @CameraID, @ClassName, @FaceName, @StartTime, @EndTime, @PointData);
	)RAW";

	std::string DeleteTrailsForClip = R"RAW(
		DELETE FROM Trail WHERE ClipUID = @ClipUID;
	)RAW";

	std::string SelectTrails = R"RAW(
		SELECT t.TrailUID, t.ClipUID, t.CameraID, t.ClassName, t.FaceName, t.StartTime, t.EndTime, t.PointData,
			   c.Timestamp AS ClipTimestamp, c.Duration AS ClipDuration
		FROM Trail t
		LEFT JOIN Clip c ON c.ClipUID = t.ClipUID
		WHERE t.CameraID = @CameraID
			AND t.EndTime >= @TimestampFrom
			AND t.StartTime <= @TimestampTo
		ORDER BY t.StartTime ASC;
	)RAW";

	std::string InsertFaceCrop= R"RAW(
		INSERT INTO FaceCrop (CameraID, Timestamp, FrameUID, TrackingID, FilePath, Confidence,
			Landmark0X, Landmark0Y, Landmark1X, Landmark1Y, Landmark2X, Landmark2Y,
			Landmark3X, Landmark3Y, Landmark4X, Landmark4Y)
		VALUES(@CameraID, @Timestamp, @FrameUID, @TrackingID, @FilePath, @Confidence,
			@Landmark0X, @Landmark0Y, @Landmark1X, @Landmark1Y, @Landmark2X, @Landmark2Y,
			@Landmark3X, @Landmark3Y, @Landmark4X, @Landmark4Y);
	)RAW";

	std::string SelectFaceCrops = R"RAW(
		SELECT CropUID, CameraID, Timestamp, FrameUID, TrackingID, FilePath, Confidence,
			Landmark0X, Landmark0Y, Landmark1X, Landmark1Y, Landmark2X, Landmark2Y,
			Landmark3X, Landmark3Y, Landmark4X, Landmark4Y
		FROM FaceCrop
		WHERE CameraID = @CameraID
			AND Timestamp >= @TimestampFrom
			AND Timestamp <= @TimestampTo
		ORDER BY Timestamp DESC;
	)RAW";

	std::string SelectRecentFaceCrops = R"RAW(
		SELECT CropUID, CameraID, Timestamp, FrameUID, TrackingID, FilePath, Confidence
		FROM FaceCrop
		ORDER BY Timestamp DESC
		LIMIT @Limit;
	)RAW";

	std::string SelectFaceCropByUID = R"RAW(
		SELECT CropUID, CameraID, Timestamp, FrameUID, TrackingID, FilePath, Confidence,
			Landmark0X, Landmark0Y, Landmark1X, Landmark1Y, Landmark2X, Landmark2Y,
			Landmark3X, Landmark3Y, Landmark4X, Landmark4Y
		FROM FaceCrop
		WHERE CropUID = @CropUID;
	)RAW";

	std::string SelectFaceCropFilePaths = R"RAW(
		SELECT FilePath FROM FaceCrop
		WHERE CameraID = @CameraID AND Timestamp < @Timestamp;
	)RAW";

	std::string DeleteFaceCropsBefore = R"RAW(
		DELETE FROM FaceCrop
		WHERE CameraID = @CameraID AND Timestamp < @Timestamp;
	)RAW";

	// -- KnownFace queries --

	std::string SelectAllKnownFaces = R"RAW(
		SELECT kf.KnownFaceUID, kf.Name, kf.Notes, kf.CreatedAt, kf.UpdatedAt,
			(SELECT COUNT(*) FROM FaceEmbedding fe WHERE fe.KnownFaceUID = kf.KnownFaceUID AND fe.Verified = 1) AS VerifiedCount,
			(SELECT COUNT(*) FROM FaceEmbedding fe WHERE fe.KnownFaceUID = kf.KnownFaceUID) AS TotalCount,
			(SELECT fc.FilePath FROM FaceEmbedding fe2
				JOIN FaceCrop fc ON fe2.FaceCropUID = fc.CropUID
				WHERE fe2.KnownFaceUID = kf.KnownFaceUID AND fe2.Verified = 1
				ORDER BY fe2.MatchConfidence DESC LIMIT 1) AS BestCropPath,
			(SELECT fe2.FaceCropUID FROM FaceEmbedding fe2
				WHERE fe2.KnownFaceUID = kf.KnownFaceUID AND fe2.Verified = 1
				ORDER BY fe2.MatchConfidence DESC LIMIT 1) AS BestCropUID
		FROM KnownFace kf
		ORDER BY kf.Name ASC;
	)RAW";

	std::string SelectKnownFaceByUID = R"RAW(
		SELECT KnownFaceUID, Name, Notes, CreatedAt, UpdatedAt
		FROM KnownFace WHERE KnownFaceUID = @KnownFaceUID;
	)RAW";

	std::string CreateKnownFace = R"RAW(
		INSERT INTO KnownFace(Name, Notes, CreatedAt, UpdatedAt)
		VALUES(@Name, @Notes, @CreatedAt, @UpdatedAt);
	)RAW";

	std::string UpdateKnownFace = R"RAW(
		UPDATE KnownFace SET Name=@Name, Notes=@Notes, UpdatedAt=@UpdatedAt
		WHERE KnownFaceUID = @KnownFaceUID;
	)RAW";

	std::string DeleteKnownFace = R"RAW(
		DELETE FROM KnownFace WHERE KnownFaceUID = @KnownFaceUID;
	)RAW";

	std::string MergeKnownFace = R"RAW(
		UPDATE FaceEmbedding SET KnownFaceUID = @TargetUID
		WHERE KnownFaceUID = @SourceUID;
	)RAW";

	// -- FaceEmbedding queries --

	std::string InsertFaceEmbedding = R"RAW(
		INSERT INTO FaceEmbedding(FaceCropUID, KnownFaceUID, Embedding, Dimension, MatchConfidence, Verified, CreatedAt)
		VALUES(@FaceCropUID, @KnownFaceUID, @Embedding, @Dimension, @MatchConfidence, @Verified, @CreatedAt);
	)RAW";

	std::string UpdateFaceEmbeddingIdentity = R"RAW(
		UPDATE FaceEmbedding SET KnownFaceUID = @KnownFaceUID, MatchConfidence = @MatchConfidence, Verified = @Verified
		WHERE EmbeddingUID = @EmbeddingUID;
	)RAW";

	std::string SelectEmbeddingByFaceCrop = R"RAW(
		SELECT EmbeddingUID, FaceCropUID, KnownFaceUID, Embedding, Dimension, MatchConfidence, Verified
		FROM FaceEmbedding WHERE FaceCropUID = @FaceCropUID;
	)RAW";

	std::string SelectVerifiedEmbeddings = R"RAW(
		SELECT fe.EmbeddingUID, fe.KnownFaceUID, fe.Embedding, fe.Dimension
		FROM FaceEmbedding fe
		WHERE fe.Verified = 1 AND fe.KnownFaceUID IS NOT NULL;
	)RAW";

	std::string SelectEmbeddingsForKnownFace = R"RAW(
		SELECT fe.EmbeddingUID, fe.FaceCropUID, fe.Embedding, fe.Dimension, fe.MatchConfidence, fe.Verified,
			fc.FilePath, fc.CameraID, fc.Timestamp, fc.Confidence AS DetectionConfidence
		FROM FaceEmbedding fe
		JOIN FaceCrop fc ON fe.FaceCropUID = fc.CropUID
		WHERE fe.KnownFaceUID = @KnownFaceUID
		ORDER BY fc.Timestamp DESC;
	)RAW";

	std::string SelectUnidentifiedFaces = R"RAW(
		SELECT fc.CropUID, fc.CameraID, fc.Timestamp, fc.FilePath, fc.Confidence,
			fe.EmbeddingUID, fe.MatchConfidence,
			fc.Landmark0X, fc.Landmark0Y, fc.Landmark1X, fc.Landmark1Y,
			fc.Landmark2X, fc.Landmark2Y, fc.Landmark3X, fc.Landmark3Y,
			fc.Landmark4X, fc.Landmark4Y
		FROM FaceCrop fc
		LEFT JOIN FaceEmbedding fe ON fc.CropUID = fe.FaceCropUID
		WHERE fe.KnownFaceUID IS NULL
		ORDER BY fc.Timestamp DESC
		LIMIT @Limit OFFSET @Offset;
	)RAW";

	std::string SelectFaceCropsWithoutEmbedding = R"RAW(
		SELECT fc.CropUID, fc.FilePath, fc.Confidence,
			fc.Landmark0X, fc.Landmark0Y, fc.Landmark1X, fc.Landmark1Y,
			fc.Landmark2X, fc.Landmark2Y, fc.Landmark3X, fc.Landmark3Y,
			fc.Landmark4X, fc.Landmark4Y, fc.CameraID, fc.Timestamp
		FROM FaceCrop fc
		LEFT JOIN FaceEmbedding fe ON fc.CropUID = fe.FaceCropUID
		WHERE fe.EmbeddingUID IS NULL
		ORDER BY fc.Timestamp ASC
		LIMIT @Limit;
	)RAW";

	std::string SelectRecognizedFacesForClip = R"RAW(
		SELECT DISTINCT kf.Name
		FROM FaceCrop fc
		INNER JOIN FaceEmbedding fe ON fc.CropUID = fe.FaceCropUID
		INNER JOIN KnownFace kf ON fe.KnownFaceUID = kf.KnownFaceUID
		WHERE fc.CameraID = @CameraID
			AND fc.Timestamp >= @TimestampFrom
			AND fc.Timestamp <= @TimestampTo;
	)RAW";

	std::string DeleteFaceEmbedding = R"RAW(
		DELETE FROM FaceEmbedding WHERE EmbeddingUID = @EmbeddingUID;
	)RAW";

	std::string SelectFaceSightings = R"RAW(
		SELECT fc.CropUID, fc.CameraID, fc.Timestamp, fc.FilePath, fc.Confidence AS DetectionConfidence,
			fe.MatchConfidence, fe.Verified, fe.EmbeddingUID
		FROM FaceEmbedding fe
		JOIN FaceCrop fc ON fe.FaceCropUID = fc.CropUID
		WHERE fe.KnownFaceUID = @KnownFaceUID
		ORDER BY fc.Timestamp DESC
		LIMIT @Limit OFFSET @Offset;
	)RAW";

#define CREATE_QUERY( X ) DB->CreateQuery( #X, X )

	std::shared_ptr<SQLiteDatabase> InitializeDatabase( std::string Filename )
	{
		auto DB = std::make_shared<SQLiteDatabase>( Filename, Database::InitializationScript, true,
			[]( const std::string& Message )
			{
				LOG_ERROR( "%s", Message.c_str() );
			}
		);

		// Schema migrations for existing databases (errors ignored if column already exists)
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Clip ADD COLUMN DetectionVersion INT DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Clip ADD COLUMN Lighting INT DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Clip ADD COLUMN Reviewed INT DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Camera ADD COLUMN ContinuousRecording INT DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Camera ADD COLUMN LowLatencyHLS INT DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE ContinuousSegment ADD COLUMN FileSize INTEGER DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE DetectionBox ADD COLUMN IsBaseline INTEGER DEFAULT 0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE DetectionBox ADD COLUMN CropPath TEXT;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE CameraAction ADD COLUMN DetectionClass TEXT DEFAULT '';", nullptr, nullptr, nullptr );
		// Recreate unique index to include DetectionClass (allows same action on same camera for different classes)
		sqlite3_exec( DB->GetDatabase(), "DROP INDEX IF EXISTS CameraActionIndex;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "CREATE UNIQUE INDEX IF NOT EXISTS CameraActionIndex ON CameraAction (ActionUID, CameraUID, DetectionClass);", nullptr, nullptr, nullptr );
		// Fix any CameraAction rows with MDThreshold=0 (would trigger on any motion)
		sqlite3_exec( DB->GetDatabase(), "UPDATE CameraAction SET MDThreshold = 0.05 WHERE MDThreshold = 0 OR MDThreshold IS NULL;", nullptr, nullptr, nullptr );
		// Action priority and cooldown
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Action ADD COLUMN Priority INTEGER DEFAULT 50;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE Action ADD COLUMN Cooldown INTEGER DEFAULT 30;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "ALTER TABLE DetectionFrame ADD COLUMN FramePath TEXT;", nullptr, nullptr, nullptr );

		// Trail table migration (new table, CREATE IF NOT EXISTS handles it)
		sqlite3_exec( DB->GetDatabase(), "CREATE TABLE IF NOT EXISTS Trail(TrailUID INTEGER PRIMARY KEY AUTOINCREMENT, ClipUID INTEGER NOT NULL, CameraID INTEGER NOT NULL, ClassName TEXT NOT NULL, FaceName TEXT, StartTime REAL NOT NULL, EndTime REAL NOT NULL, PointData TEXT NOT NULL, FOREIGN KEY(ClipUID) REFERENCES Clip(ClipUID) ON DELETE CASCADE);", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "CREATE INDEX IF NOT EXISTS idx_trail_camera_time ON Trail(CameraID, StartTime, EndTime);", nullptr, nullptr, nullptr );

		// Fix FaceCrop rows with bad confidence values from column-14 bug (landmark pixel coords stored as confidence)
		sqlite3_exec( DB->GetDatabase(), "UPDATE FaceCrop SET Confidence = Confidence / 10.0 WHERE Confidence > 1.0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "UPDATE FaceCrop SET Confidence = Confidence / 10.0 WHERE Confidence > 1.0;", nullptr, nullptr, nullptr );
		sqlite3_exec( DB->GetDatabase(), "UPDATE FaceCrop SET Confidence = Confidence / 10.0 WHERE Confidence > 1.0;", nullptr, nullptr, nullptr );

		// Migrate old Tag table schema — drop and recreate if it has the old schema
		// (old schema had Name CHAR(64), Description TEXT; new needs Name TEXT UNIQUE, Display, Icon, etc.)
		{
			char* errMsg = nullptr;
			// Check if old schema (has Description column but no Display column)
			int hasDisplay = 0;
			sqlite3_exec( DB->GetDatabase(), "SELECT Display FROM Tag LIMIT 1;",
				[]( void* data, int, char**, char** ) -> int { *(int*)data = 1; return 0; },
				&hasDisplay, &errMsg );
			if( errMsg ) { sqlite3_free( errMsg ); errMsg = nullptr; }

			if( !hasDisplay )
			{
				// Old schema — drop and recreate
				sqlite3_exec( DB->GetDatabase(), "DROP TABLE IF EXISTS Tag;", nullptr, nullptr, nullptr );
				sqlite3_exec( DB->GetDatabase(), R"(
					CREATE TABLE IF NOT EXISTS Tag(
						TagUID INTEGER PRIMARY KEY AUTOINCREMENT,
						Name TEXT UNIQUE NOT NULL,
						Display TEXT NOT NULL,
						Icon TEXT DEFAULT '',
						SortOrder INTEGER DEFAULT 0,
						Hidden INTEGER DEFAULT 0
					);
				)", nullptr, nullptr, nullptr );
			}
		}

		// Ensure ClipTag and CameraTagExclusion tables exist (no-op on fresh installs)
		sqlite3_exec( DB->GetDatabase(), R"(
			CREATE TABLE IF NOT EXISTS ClipTag(
				ClipUID INTEGER NOT NULL,
				TagUID INTEGER NOT NULL,
				PRIMARY KEY (ClipUID, TagUID),
				FOREIGN KEY (ClipUID) REFERENCES Clip(ClipUID) ON DELETE CASCADE,
				FOREIGN KEY (TagUID) REFERENCES Tag(TagUID)
			);
			CREATE INDEX IF NOT EXISTS ClipTagByTag ON ClipTag (TagUID);
			CREATE TABLE IF NOT EXISTS CameraTagExclusion(
				CameraID INTEGER NOT NULL,
				TagUID INTEGER NOT NULL,
				PRIMARY KEY (CameraID, TagUID),
				FOREIGN KEY (TagUID) REFERENCES Tag(TagUID)
			);
		)", nullptr, nullptr, nullptr );

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
		CREATE_QUERY( SetUserPassword );

		CREATE_QUERY( FindSession );
		CREATE_QUERY( VerifySession );
		CREATE_QUERY( VerifySessionAndCSRF );
		CREATE_QUERY( CreateSession );
		CREATE_QUERY( DeleteSession );
		CREATE_QUERY( DeleteUserSessions );

		CREATE_QUERY( GetUserCount );

		CREATE_QUERY( CreateCamera );
		CREATE_QUERY( UpdateCamera );
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
		CREATE_QUERY( SelectClipForReprocess );
		CREATE_QUERY( UpdateClipDetection );
		CREATE_QUERY( ResetClipDetection );
		CREATE_QUERY( ResetClipDetectionBulk );
		CREATE_QUERY( CountClipsToReprocess );
		CREATE_QUERY( SelectReprocessQueue );
		CREATE_QUERY( SelectClipsNeedingLighting );
		CREATE_QUERY( UpdateClipLighting );
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

		CREATE_QUERY( FindOrCreateTag );
		CREATE_QUERY( SelectTagByName );
		CREATE_QUERY( SelectAllTags );
		CREATE_QUERY( UpdateTag );
		CREATE_QUERY( InsertClipTag );
		CREATE_QUERY( DeleteClipTags );
		CREATE_QUERY( SelectTagsForClip );
		CREATE_QUERY( SelectCameraTagExclusions );
		CREATE_QUERY( InsertCameraTagExclusion );
		CREATE_QUERY( DeleteCameraTagExclusions );
		CREATE_QUERY( SetClipReviewed );
		CREATE_QUERY( SetAllClipsReviewed );
		CREATE_QUERY( SelectRecentUnreviewed );
		CREATE_QUERY( SelectClipCountsByDay );
		CREATE_QUERY( SelectClipsForTimeline );
		CREATE_QUERY( SelectClipsWithTags );
		CREATE_QUERY( ClearLegacyClipTags );

		CREATE_QUERY( FindActions );
		CREATE_QUERY( GetAction );
		CREATE_QUERY( FindDetectionActions );
		CREATE_QUERY( SelectAllActions );
		CREATE_QUERY( CreateAction );
		CREATE_QUERY( UpdateAction );
		CREATE_QUERY( DeleteAction );
		CREATE_QUERY( DeleteCameraActionsForAction );
		CREATE_QUERY( SelectCameraActionsForAction );
		CREATE_QUERY( SelectAllCameraActions );
		CREATE_QUERY( CreateCameraAction );
		CREATE_QUERY( DeleteCameraAction );

		CREATE_QUERY( CreateContinuousSegment );
		CREATE_QUERY( SelectContinuousSegments );
		CREATE_QUERY( SelectContinuousSegmentsToDelete );
		CREATE_QUERY( DeleteContinuousSegment );
		CREATE_QUERY( SelectContinuousTotalSize );
		CREATE_QUERY( SelectContinuousSizePerCamera );
		CREATE_QUERY( SelectContinuousSegmentsWithNoFileSize );
		CREATE_QUERY( UpdateContinuousSegmentFileSize );
		CREATE_QUERY( SelectContinuousSegmentByFilePath );
		CREATE_QUERY( SelectOldestContinuousSegment );
		CREATE_QUERY( SelectContinuousCoverage );
		CREATE_QUERY( SelectContinuousSegmentAtTimestamp );

		CREATE_QUERY( InsertDetectionFrame );
		CREATE_QUERY( InsertDetectionBox );
		CREATE_QUERY( SelectDetectionFramesWithBoxes );
		CREATE_QUERY( DeleteDetectionFramesBefore );
		CREATE_QUERY( DeleteAllDetectionFrames );
		CREATE_QUERY( DeleteDetectionFramesInRange );

		CREATE_QUERY( InsertTrail );
		CREATE_QUERY( DeleteTrailsForClip );
		CREATE_QUERY( SelectTrails );

		CREATE_QUERY( InsertFaceCrop );
		CREATE_QUERY( SelectFaceCrops );
		CREATE_QUERY( SelectRecentFaceCrops );
		CREATE_QUERY( SelectFaceCropByUID );
		CREATE_QUERY( SelectFaceCropFilePaths );
		CREATE_QUERY( DeleteFaceCropsBefore );

		// Face recognition queries
		CREATE_QUERY( SelectAllKnownFaces );
		CREATE_QUERY( SelectKnownFaceByUID );
		CREATE_QUERY( CreateKnownFace );
		CREATE_QUERY( UpdateKnownFace );
		CREATE_QUERY( DeleteKnownFace );
		CREATE_QUERY( MergeKnownFace );
		CREATE_QUERY( InsertFaceEmbedding );
		CREATE_QUERY( UpdateFaceEmbeddingIdentity );
		CREATE_QUERY( SelectEmbeddingByFaceCrop );
		CREATE_QUERY( SelectVerifiedEmbeddings );
		CREATE_QUERY( SelectEmbeddingsForKnownFace );
		CREATE_QUERY( SelectUnidentifiedFaces );
		CREATE_QUERY( SelectFaceCropsWithoutEmbedding );
		CREATE_QUERY( SelectRecognizedFacesForClip );
		CREATE_QUERY( DeleteFaceEmbedding );
		CREATE_QUERY( SelectFaceSightings );

		return DB;
	}

	bool HasAdminUser( const std::shared_ptr<SQLiteDatabase>& DB )
	{
		bool hasAdmin = false;

		SQLiteDatabaseQueryInstance query( DB, "GetUserCount" );
		query->Execute( [&hasAdmin]( const SQLiteDatabaseQuery& q )
		{
			hasAdmin = q.GetColumnValueInt( 0 ) > 0;
			return true;
		});

		return hasAdmin;
	}
}