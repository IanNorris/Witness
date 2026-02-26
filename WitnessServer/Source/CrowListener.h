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

class CrowListener
{
public:
	CrowListener( const std::string& Hostname, int Port, bool Secure, DebugConsole* DebugConsoleInstance );
	virtual ~CrowListener();

	void Initialise( const std::unordered_map< StringT, StringT >& Settings );

	void Start();

	void Stop();

	const std::shared_ptr<GlobalContext>& GetGlobalContext() { return m_GlobalContext; }

	const StringT& GetBaseUri() { return m_BaseUri; }

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

	// Clips
	void HandleClipThumbnail( const crow::request& req, crow::response& res, int cameraId, const std::string& clipId, bool video );
	void HandleClipEnum( const crow::request& req, crow::response& res, int cameraId, int maxCount, const std::string& startDate, const std::string& rangePeriod, int pageOffset );
	void HandleClipToggleSave( const crow::request& req, crow::response& res );
	void HandleClipDelete( const crow::request& req, crow::response& res );

	// Groups
	void HandleGroupEnum( const crow::request& req, crow::response& res );
	void HandleGroupCreate( const crow::request& req, crow::response& res );
	void HandleGroupUpdate( const crow::request& req, crow::response& res );
	void HandleGroupDelete( const crow::request& req, crow::response& res );

	// Debug
	void HandleDebugEnum( const crow::request& req, crow::response& res );
	void HandleDebugSet( const crow::request& req, crow::response& res );
	void HandleDebugReset( const crow::request& req, crow::response& res );

	crow::SimpleApp m_App;
	std::thread m_ServerThread;

	std::shared_ptr<GlobalContext> m_GlobalContext;
	DebugConsole* m_DebugConsole;

	std::unordered_map<std::string, std::string> m_StaticFiles; // relative path -> content type
	std::string m_StaticRoot;

	StringT m_BaseUri;
	std::string m_Hostname;
	int m_Port;
	bool m_Secure;
};
