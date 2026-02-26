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

	// Camera preview
	void HandlePreview( const crow::request& req, crow::response& res, int cameraId, bool largePreview );

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
