#pragma once

#define WITNESS_LISTENER_VERSION "0.0.0.2"

#include <unordered_map>
#include <string>
#include <memory>
#include <thread>

#include "crow.h"

#include "Common.h"
#include "GlobalContext.h"
#include "DebugBind.h"

struct SecurityHeadersMiddleware
{
	struct context {};

	void before_handle( crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/ ) {}

	void after_handle( crow::request& /*req*/, crow::response& res, context& /*ctx*/ )
	{
		static const char* CSP =
			"default-src 'self'; "
			"connect-src 'self' ws: wss:; "
			"script-src 'self' 'unsafe-inline' 'unsafe-eval' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
			"worker-src 'self' blob:; "
			"style-src 'self' 'unsafe-inline' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
			"script-src-elem 'self' 'unsafe-inline' 'unsafe-eval' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
			"style-src-attr 'self' 'unsafe-inline' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
			"img-src 'self' data: blob: https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
			"font-src 'self' data:; "
			"media-src 'self' blob:;";

		res.set_header( "Content-Security-Policy", CSP );
		res.set_header( "X-Content-Type-Options", "nosniff" );
		res.set_header( "X-Frame-Options", "DENY" );
		res.set_header( "Referrer-Policy", "strict-origin-when-cross-origin" );
		res.set_header( "Strict-Transport-Security", "max-age=31536000; includeSubDomains" );
	}
};

using WitnessApp = crow::App<SecurityHeadersMiddleware>;

class CrowListener
{
public:
	CrowListener( const std::string& Hostname, int Port, bool Secure,
	              const std::string& CertPath, const std::string& KeyPath,
	              DebugConsole* DebugConsoleInstance );
	virtual ~CrowListener();

	void Initialise( const std::unordered_map< std::string, std::string >& Settings );

	void Start();

	void Stop();

	// Reload TLS certificate and key files (graceful restart)
	bool ReloadTLS();

	const std::shared_ptr<GlobalContext>& GetGlobalContext() { return m_GlobalContext; }

	const std::string& GetBaseUri() { return m_BaseUri; }

	// Re-read build hash from disk (for auto-refresh detection)
	void ReadBuildHash();

private:

	void RegisterRoutes();

	// Static file serving
	void ServeStaticFile( const crow::request& req, crow::response& res, const std::string& path );

	// HLS streaming
	void HandlePlaylist( const crow::request& req, crow::response& res, int cameraId );
	void HandleSegment( const crow::request& req, crow::response& res, int cameraId, int segmentId, const std::string& partId );

	// Camera
	void HandlePreview( const crow::request& req, crow::response& res, int cameraId, bool largePreview );
	void HandleCameraEnum( const crow::request& req, crow::response& res, bool asAdmin, bool longPoll );
	void HandleCameraRecord( const crow::request& req, crow::response& res, int cameraId );
	void HandleCameraCreate( const crow::request& req, crow::response& res );
	void HandleCameraUpdate( const crow::request& req, crow::response& res );
	void HandleCameraDelete( const crow::request& req, crow::response& res );
	void HandleCameraSetGroups( const crow::request& req, crow::response& res );
	void HandleCameraResetStats( const crow::request& req, crow::response& res );

	// Auth
	void HandleAuthLogin( const crow::request& req, crow::response& res );
	void HandleAuthLogout( const crow::request& req, crow::response& res );
	void HandleAuthGetProfile( const crow::request& req, crow::response& res );
	void HandleAuthEnumUsers( const crow::request& req, crow::response& res );
	void HandleAuthNewUser( const crow::request& req, crow::response& res );
	void HandleAuthChangePassword( const crow::request& req, crow::response& res );
	void HandleAuthToggleEnabled( const crow::request& req, crow::response& res );
	void HandleAuthToggleAdmin( const crow::request& req, crow::response& res );
	void HandleAuthSetDisplayName( const crow::request& req, crow::response& res );
	void HandleAuthSetUserGroups( const crow::request& req, crow::response& res );
	void HandleAuthClearSessions( const crow::request& req, crow::response& res );

	// Clips
	void HandleClipThumbnail( const crow::request& req, crow::response& res, int cameraId, const std::string& clipId, bool video );
	void HandleClipEnum( const crow::request& req, crow::response& res, int cameraId, int maxCount, const std::string& startDate, const std::string& rangePeriod, int pageOffset );
	void HandleClipToggleSave( const crow::request& req, crow::response& res );
	void HandleClipDelete( const crow::request& req, crow::response& res );
	void HandleClipRetag( const crow::request& req, crow::response& res );
	void HandleClipReview( const crow::request& req, crow::response& res );
	void HandleClipRecent( const crow::request& req, crow::response& res, int maxCount );
	void HandleClipCalendar( const crow::request& req, crow::response& res, int year, int month );
	void HandleClipTimeline( const crow::request& req, crow::response& res, const std::string& fromStr, const std::string& toStr );

	// Tags
	void HandleTagEnum( const crow::request& req, crow::response& res );
	void HandleTagUpdate( const crow::request& req, crow::response& res );
	void HandleCameraTagExclusions( const crow::request& req, crow::response& res, int cameraId );

	// Groups
	void HandleGroupEnum( const crow::request& req, crow::response& res );
	void HandleGroupCreate( const crow::request& req, crow::response& res );
	void HandleGroupUpdate( const crow::request& req, crow::response& res );
	void HandleGroupDelete( const crow::request& req, crow::response& res );

	// Actions (notification triggers)
	void HandleActionEnum( const crow::request& req, crow::response& res );
	void HandleActionCreate( const crow::request& req, crow::response& res );
	void HandleActionUpdate( const crow::request& req, crow::response& res );
	void HandleActionDelete( const crow::request& req, crow::response& res );
	void HandleActionAssign( const crow::request& req, crow::response& res );
	void HandleActionUnassign( const crow::request& req, crow::response& res );
	void HandleActionSounds( const crow::request& req, crow::response& res );
	void HandleActionTestSound( const crow::request& req, crow::response& res );

	// Debug
	void HandleDebugEnum( const crow::request& req, crow::response& res );
	void HandleDebugSet( const crow::request& req, crow::response& res );
	void HandleDebugReset( const crow::request& req, crow::response& res );
	void HandleDebugReloadTLS( const crow::request& req, crow::response& res );
	void HandleDebugStreamingDiag( const crow::request& req, crow::response& res );
	void HandleDebugDisk( const crow::request& req, crow::response& res );
	void HandleDebugDiskScan( const crow::request& req, crow::response& res );
	void HandleDetectionQuery( const crow::request& req, crow::response& res, int cameraId );

	// Face detection / crops
	void HandleFaceQuery( const crow::request& req, crow::response& res, int cameraId );
	void HandleFaceRecent( const crow::request& req, crow::response& res );
	void HandleFaceCropImage( const crow::request& req, crow::response& res, int cropUID );
	void HandleDetectionCropImage( const crow::request& req, crow::response& res, const std::string& cropPath );

	// Face recognition
	void HandleKnownFaceList( const crow::request& req, crow::response& res );
	void HandleKnownFaceCreate( const crow::request& req, crow::response& res );
	void HandleKnownFaceUpdate( const crow::request& req, crow::response& res );
	void HandleKnownFaceDelete( const crow::request& req, crow::response& res );
	void HandleFaceAssign( const crow::request& req, crow::response& res );
	void HandleFaceUnassign( const crow::request& req, crow::response& res );
	void HandleFaceMerge( const crow::request& req, crow::response& res );
	void HandleUnidentifiedFaces( const crow::request& req, crow::response& res );
	void HandleFaceSightings( const crow::request& req, crow::response& res, int knownFaceId );
	void HandleFaceReprocess( const crow::request& req, crow::response& res );
	std::shared_ptr<Witness::Camera::FaceEmbeddingModel> EnsureFaceModel();

	// Setup (reconfiguration)
	void HandleSetupPage( const crow::request& req, crow::response& res );
	void HandleSetupSettings( const crow::request& req, crow::response& res );
	void HandleSetupApply( const crow::request& req, crow::response& res );
	void HandleSetupTestCuda( const crow::request& req, crow::response& res );
	void HandleSettingsSet( const crow::request& req, crow::response& res );

	// DVR (continuous recording playback)
	void HandleDvrCoverage( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr );
	void HandleDvrSegment( const crow::request& req, crow::response& res, int segmentId );
	void HandleDvrPlaylist( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr );
	void HandleDvrSegments( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr );
	void HandleDvrThumbnail( const crow::request& req, crow::response& res, int cameraId, const std::string& timestampStr );

	bool ConfigureSSL();

	WitnessApp m_App;
	std::thread m_ServerThread;

	std::shared_ptr<GlobalContext> m_GlobalContext;
	DebugConsole* m_DebugConsole;

	std::unordered_map<std::string, std::string> m_StaticFiles; // relative path -> content type
	std::unordered_map<std::string, std::string> m_FileCache;  // relative path -> file content
	std::mutex m_FileCacheMutex;
	std::string m_StaticRoot;

	std::string m_BaseUri;
	std::string m_Hostname;
	int m_Port;
	uint32_t m_CrowThreadCount;
	bool m_Secure;
	std::string m_CertPath;
	std::string m_KeyPath;

	// Background cert file monitor
	std::thread m_CertMonitorThread;
	std::atomic<bool> m_CertMonitorRunning{ false };
	std::filesystem::file_time_type m_LastCertModTime;
	void CertMonitorLoop();
};
