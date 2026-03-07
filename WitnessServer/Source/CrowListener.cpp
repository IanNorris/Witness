#include "CrowListener.h"
#include "CrowAuth.h"
#include "CameraWorker.h"
#include "Messages.h"
#include "AuthHelpers.h"

#include <Log.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <format>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

#include "sodium.h"

namespace fs = std::filesystem;

// Route Crow's internal logging through Witness Log framework.
// Downgrades known-benign SSL handshake errors to Debug level.
class WitnessCrowLogHandler : public crow::ILogHandler
{
public:
	void log( std::string message, crow::LogLevel level ) override
	{
		// SSL handshake failures are benign (scanners, self-signed cert rejection)
		if( message.find( "Could not start adaptor" ) != std::string::npos )
		{
			LOG_DEBUG( "Crow: %s", message.c_str() );
			return;
		}

		switch( level )
		{
			case crow::LogLevel::Debug:    LOG_DEBUG( "Crow: %s", message.c_str() ); break;
			case crow::LogLevel::Info:     LOG_INFO( "Crow: %s", message.c_str() ); break;
			case crow::LogLevel::Warning:  LOG_WARNING( "Crow: %s", message.c_str() ); break;
			case crow::LogLevel::Error:
			case crow::LogLevel::Critical: LOG_ERROR( "Crow: %s", message.c_str() ); break;
		}
	}
};

static WitnessCrowLogHandler s_CrowLogHandler;

CrowListener::CrowListener( const std::string& Hostname, int Port, bool Secure,
                            const std::string& CertPath, const std::string& KeyPath,
                            DebugConsole* DebugConsoleInstance )
: m_DebugConsole( DebugConsoleInstance )
, m_Hostname( Hostname )
, m_Port( Port )
, m_Secure( Secure )
, m_CertPath( CertPath )
, m_KeyPath( KeyPath )
{
	crow::logger::setHandler( &s_CrowLogHandler );

	m_GlobalContext = std::make_unique<GlobalContext>();
	m_GlobalContext->Port = Port;

	std::string Scheme = Secure ? "https" : "http";
	m_BaseUri = Scheme + "://" + Hostname + ":" + std::to_string( Port );
}

CrowListener::~CrowListener()
{
	Stop();
}

void CrowListener::Initialise( const std::unordered_map< std::string, std::string >& Settings )
{
	// Derive static file root from exe location
	std::filesystem::path exePath;
#ifdef _WIN32
	wchar_t exeBuf[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, exeBuf, MAX_PATH );
	exePath = std::filesystem::path( exeBuf ).parent_path();
#else
	exePath = std::filesystem::canonical( "/proc/self/exe" ).parent_path();
#endif
	m_StaticRoot = ( exePath / "Web" ).string();

	// Build static file map
	std::unordered_map<std::string, std::string> MimeTypes;
	MimeTypes["css"] = "text/css";
	MimeTypes["html"] = "text/html";
	MimeTypes["js"] = "application/javascript";
	MimeTypes["svg"] = "image/svg+xml";
	MimeTypes["png"] = "image/png";
	MimeTypes["jpg"] = "image/jpeg";
	MimeTypes["ico"] = "image/x-icon";
	MimeTypes["woff"] = "font/woff";
	MimeTypes["woff2"] = "font/woff2";
	MimeTypes["ttf"] = "font/ttf";
	MimeTypes["eot"] = "application/vnd.ms-fontobject";
	MimeTypes["map"] = "application/json";

	std::error_code ec;
	for( auto& Entry : fs::recursive_directory_iterator( m_StaticRoot, ec ) )
	{
		if( !fs::is_directory( Entry ) )
		{
			auto RelPath = fs::relative( Entry.path(), m_StaticRoot );
			std::string PathStr = RelPath.generic_string();

			std::string ContentType = "application/octet-stream";
			if( Entry.path().has_extension() )
			{
				std::string Ext = Entry.path().extension().string().substr(1);
				auto It = MimeTypes.find( Ext );
				if( It != MimeTypes.end() )
				{
					ContentType = It->second;
				}
			}

			m_StaticFiles[PathStr] = ContentType;
		}
	}

	if( ec )
	{
		LOG_ERROR( "Static file scan error: %s", ec.message().c_str() );
	}

	LOG_INFO( "Static root: %s (%zu files)", m_StaticRoot.c_str(), m_StaticFiles.size() );

	// Read build hash for auto-refresh detection
	ReadBuildHash();

	RegisterRoutes();
}

void CrowListener::RegisterRoutes()
{
	// HLS playlist: /stream/<cameraId>
	CROW_ROUTE( m_App, "/stream/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePlaylist( req, res, cameraId );
	});

	// HLS segment: /stream/<cameraId>/<segmentId>/<partId>
	CROW_ROUTE( m_App, "/stream/<int>/<int>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, int segmentId, const std::string& partId )
	{
		HandleSegment( req, res, cameraId, segmentId, partId );
	});

	// Camera preview
	CROW_ROUTE( m_App, "/camera/preview/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePreview( req, res, cameraId, false );
	});

	CROW_ROUTE( m_App, "/camera/previewLarge/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePreview( req, res, cameraId, true );
	});

	// Camera enum
	CROW_ROUTE( m_App, "/camera/enum" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraEnum( req, res, false, false );
	});

	CROW_ROUTE( m_App, "/camera/enum_longpoll" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraEnum( req, res, false, true );
	});

	CROW_ROUTE( m_App, "/camera/admin_enum" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraEnum( req, res, true, false );
	});

	// Camera POST actions
	CROW_ROUTE( m_App, "/camera/record/<int>" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandleCameraRecord( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/camera/admin_create" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraCreate( req, res );
	});

	CROW_ROUTE( m_App, "/camera/admin_update" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraUpdate( req, res );
	});

	CROW_ROUTE( m_App, "/camera/admin_delete" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraDelete( req, res );
	});

	CROW_ROUTE( m_App, "/camera/set_groups" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraSetGroups( req, res );
	});

	CROW_ROUTE( m_App, "/camera/admin_reset_stats" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleCameraResetStats( req, res );
	});

	// Auth
	CROW_ROUTE( m_App, "/auth/login" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthLogin( req, res );
	});

	CROW_ROUTE( m_App, "/auth/logout" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthLogout( req, res );
	});

	CROW_ROUTE( m_App, "/auth/profile" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthGetProfile( req, res );
	});

	CROW_ROUTE( m_App, "/auth/admin_enum" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthEnumUsers( req, res );
	});

	CROW_ROUTE( m_App, "/auth/new_user" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthNewUser( req, res );
	});

	CROW_ROUTE( m_App, "/auth/change_password" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthChangePassword( req, res );
	});

	CROW_ROUTE( m_App, "/auth/toggle_enabled" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthToggleEnabled( req, res );
	});

	CROW_ROUTE( m_App, "/auth/toggle_admin" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthToggleAdmin( req, res );
	});

	CROW_ROUTE( m_App, "/auth/set_display_name" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthSetDisplayName( req, res );
	});

	CROW_ROUTE( m_App, "/auth/set_user_groups" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthSetUserGroups( req, res );
	});

	CROW_ROUTE( m_App, "/auth/clear_sessions" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthClearSessions( req, res );
	});

	// Clips
	CROW_ROUTE( m_App, "/clip/thumb/<int>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& clipId )
	{
		HandleClipThumbnail( req, res, cameraId, clipId, false );
	});

	CROW_ROUTE( m_App, "/clip/video/<int>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& clipId )
	{
		HandleClipThumbnail( req, res, cameraId, clipId, true );
	});

	CROW_ROUTE( m_App, "/clip/enum/<int>/<int>/<string>/<string>/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, int maxCount, const std::string& startDate, const std::string& rangePeriod, int pageOffset )
	{
		HandleClipEnum( req, res, cameraId, maxCount, startDate, rangePeriod, pageOffset );
	});

	CROW_ROUTE( m_App, "/clip/toggleSave" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleClipToggleSave( req, res );
	});

	CROW_ROUTE( m_App, "/clip/delete" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleClipDelete( req, res );
	});

	CROW_ROUTE( m_App, "/clip/retag" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleClipRetag( req, res );
	});

	CROW_ROUTE( m_App, "/clip/review" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleClipReview( req, res );
	});

	CROW_ROUTE( m_App, "/clip/recent/<int>" )
	([this]( const crow::request& req, crow::response& res, int maxCount )
	{
		HandleClipRecent( req, res, maxCount );
	});

	CROW_ROUTE( m_App, "/clip/calendar/<int>/<int>" )
	([this]( const crow::request& req, crow::response& res, int year, int month )
	{
		HandleClipCalendar( req, res, year, month );
	});

	CROW_ROUTE( m_App, "/clip/timeline/<string>/<string>" )
	([this]( const crow::request& req, crow::response& res, const std::string& fromStr, const std::string& toStr )
	{
		HandleClipTimeline( req, res, fromStr, toStr );
	});

	// Tags
	CROW_ROUTE( m_App, "/clip/tags" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleTagEnum( req, res );
	});

	CROW_ROUTE( m_App, "/clip/tags/update" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleTagUpdate( req, res );
	});

	CROW_ROUTE( m_App, "/clip/tags/camera-exclusions/<int>" ).methods( crow::HTTPMethod::GET, crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandleCameraTagExclusions( req, res, cameraId );
	});

	// Groups
	CROW_ROUTE( m_App, "/group/enum" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleGroupEnum( req, res );
	});

	CROW_ROUTE( m_App, "/group/create" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleGroupCreate( req, res );
	});

	CROW_ROUTE( m_App, "/group/update" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleGroupUpdate( req, res );
	});

	CROW_ROUTE( m_App, "/group/delete" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleGroupDelete( req, res );
	});

	// Debug
	CROW_ROUTE( m_App, "/debug/enum" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugEnum( req, res );
	});

	CROW_ROUTE( m_App, "/debug/set" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugSet( req, res );
	});

	CROW_ROUTE( m_App, "/debug/reset" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugReset( req, res );
	});

	CROW_ROUTE( m_App, "/debug/reload_tls" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugReloadTLS( req, res );
	});

	CROW_ROUTE( m_App, "/debug/streaming" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugStreamingDiag( req, res );
	});

	CROW_ROUTE( m_App, "/debug/disk" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugDisk( req, res );
	});

	CROW_ROUTE( m_App, "/debug/disk/scan" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugDiskScan( req, res );
	});

	CROW_ROUTE( m_App, "/api/detection/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandleDetectionQuery( req, res, cameraId );
	});

	// Setup / Reconfiguration (admin-only)
	CROW_ROUTE( m_App, "/setup" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleSetupPage( req, res );
	});

	CROW_ROUTE( m_App, "/setup/<path>" )
	([this]( const crow::request& req, crow::response& res, const std::string& path )
	{
		HandleSetupPage( req, res );
	});

	CROW_ROUTE( m_App, "/api/setup/settings" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleSetupSettings( req, res );
	});

	CROW_ROUTE( m_App, "/api/setup/apply" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleSetupApply( req, res );
	});

	CROW_ROUTE( m_App, "/api/setup/test-cuda" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleSetupTestCuda( req, res );
	});

	CROW_ROUTE( m_App, "/api/settings/set" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleSettingsSet( req, res );
	});

	// DVR (continuous recording playback)
	CROW_ROUTE( m_App, "/dvr/coverage/<int>/<string>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& from, const std::string& to )
	{
		HandleDvrCoverage( req, res, cameraId, from, to );
	});

	CROW_ROUTE( m_App, "/dvr/segment/<int>" )
	([this]( const crow::request& req, crow::response& res, int segmentId )
	{
		HandleDvrSegment( req, res, segmentId );
	});

	CROW_ROUTE( m_App, "/dvr/playlist/<int>/<string>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& from, const std::string& to )
	{
		HandleDvrPlaylist( req, res, cameraId, from, to );
	});

	CROW_ROUTE( m_App, "/dvr/segments/<int>/<string>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& from, const std::string& to )
	{
		HandleDvrSegments( req, res, cameraId, from, to );
	});

	CROW_ROUTE( m_App, "/dvr/thumbnail/<int>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& timestamp )
	{
		HandleDvrThumbnail( req, res, cameraId, timestamp );
	});

	// WebSocket MSE stream
	CROW_WEBSOCKET_ROUTE( m_App, "/ws/stream/<int>" )
		.onaccept([this]( const crow::request& req, void** userdata ) -> bool
		{
			// Authenticate
			int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
				CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
			if( UserUID < 0 ) return false;

			// Extract camera ID from URL (last path segment)
			std::string url = req.url;
			auto lastSlash = url.rfind('/');
			if( lastSlash == std::string::npos ) return false;
			int cameraId = std::atoi( url.substr( lastSlash + 1 ).c_str() );

			auto* state = m_GlobalContext->FindCameraById( cameraId );
			if( !state || !state->Worker || !state->Worker->GetLiveStream() )
				return false;

			// Pass camera ID via userdata
			*userdata = reinterpret_cast<void*>( static_cast<intptr_t>( cameraId ) );
			return true;
		})
		.onopen([this]( crow::websocket::connection& conn )
		{
			int cameraId = static_cast<int>( reinterpret_cast<intptr_t>( conn.userdata() ) );

			m_GlobalContext->Streams->Subscribe( cameraId, &conn );

			auto* state = m_GlobalContext->FindCameraById( cameraId );
			if( !state || !state->Worker ) return;

			auto& liveStream = state->Worker->GetLiveStream();
			if( !liveStream ) return;

			int generation = liveStream->GetInitGeneration();
			auto initData = liveStream->GetInitSegment();

			// Send init segment — client waits for next live keyframe to start
			if( initData && !initData->empty() )
			{
				crow::json::wvalue ctrl;
				ctrl["type"] = "initSegment";
				ctrl["generation"] = generation;
				m_GlobalContext->Streams->SendControlDirect( &conn, ctrl.dump() );
				m_GlobalContext->Streams->SendBinaryDirect( &conn, initData );
			}

			LOG_INFO( "[MSE] Stream client connected for camera %d", cameraId );
		})
		.onclose([this]( crow::websocket::connection& conn, const std::string& /*reason*/ )
		{
			m_GlobalContext->Streams->Unsubscribe( &conn );
		})
		.onmessage([this]( crow::websocket::connection& /*conn*/, const std::string& /*data*/, bool /*is_binary*/ )
		{
			// Client->server messages not used for MSE streaming
		});

	// WebSocket event stream
	CROW_WEBSOCKET_ROUTE( m_App, "/ws/events" )
		.onaccept([this]( const crow::request& req, void** ) -> bool
		{
			// Reject unauthenticated WebSocket connections
			int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
				CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
			return UserUID >= 0;
		})
		.onopen([this]( crow::websocket::connection& conn )
		{
			m_GlobalContext->Events->AddConnection( &conn );

			// Send initial state: all cameras with recording status
			crow::json::wvalue initData;
			std::vector<crow::json::wvalue> cams;
			{
				std::shared_lock<std::shared_mutex> lock( m_GlobalContext->Mutex );
				for( auto& [id, state] : m_GlobalContext->GetCameraMap() )
				{
					crow::json::wvalue cam;
					cam["cameraID"] = id;
					cam["name"] = state.Name;
					cam["status"] = state.Status;
					cam["recording"] = state.IsRecording;
					cams.push_back( std::move( cam ) );
				}
			}
			initData["cameras"] = std::move( cams );
			if( !m_GlobalContext->BuildHash.empty() )
				initData["buildHash"] = m_GlobalContext->BuildHash;

			crow::json::wvalue envelope;
			envelope["event"] = "init";
			envelope["data"] = std::move( initData );
			conn.send_text( envelope.dump() );
		})
		.onclose([this]( crow::websocket::connection& conn, const std::string& /*reason*/ )
		{
			m_GlobalContext->Events->RemoveConnection( &conn );
		})
		.onmessage([this]( crow::websocket::connection& /*conn*/, const std::string& /*data*/, bool /*is_binary*/ )
		{
			// Client->server messages not used; REST handles mutations
		});

	// Catch-all route for static files (lowest priority)
	CROW_CATCHALL_ROUTE( m_App )
	([this]( crow::response& res )
	{
		res.code = 404;
	});

	// Static file routes — serve Vue SPA
	CROW_ROUTE( m_App, "/" )
	([this]( const crow::request& req, crow::response& res )
	{
		ServeStaticFile( req, res, "index.html" );
		res.end();
	});

	CROW_ROUTE( m_App, "/<path>" )
	([this]( const crow::request& req, crow::response& res, const std::string& path )
	{
		ServeStaticFile( req, res, path );
		res.end();
	});
}

void CrowListener::ServeStaticFile( const crow::request& req, crow::response& res, const std::string& path )
{
	std::string lookup = path;

	// Strip trailing slash
	while( !lookup.empty() && lookup.back() == '/' )
		lookup.pop_back();

	if( lookup.empty() )
		lookup = "index.html";

	auto serveFromCacheOrDisk = [&]( const std::string& key ) -> bool
	{
		auto it = m_StaticFiles.find( key );
		if( it == m_StaticFiles.end() ) return false;

		// Check cache first
		{
			std::lock_guard<std::mutex> lock( m_FileCacheMutex );
			auto cacheIt = m_FileCache.find( key );
			if( cacheIt != m_FileCache.end() )
			{
				res.set_header( "Content-Type", it->second );
				res.body = cacheIt->second;
				res.code = 200;
				return true;
			}
		}

		// Read from disk and cache
		fs::path fullPath = m_StaticRoot;
		fullPath /= it->first;

		std::ifstream file( fullPath, std::ios::binary );
		if( !file ) return false;

		std::string body( (std::istreambuf_iterator<char>(file)),
						  std::istreambuf_iterator<char>() );

		{
			std::lock_guard<std::mutex> lock( m_FileCacheMutex );
			m_FileCache[key] = body;
		}

		res.set_header( "Content-Type", it->second );
		res.body = std::move( body );
		res.code = 200;
		return true;
	};

	for( int pass = 0; pass < 2; pass++ )
	{
		if( serveFromCacheOrDisk( lookup ) ) return;

		if( pass == 0 )
		{
			lookup += "/index.html";
		}
	}

	// SPA fallback: Vue Router paths should serve index.html
	if( !path.empty() && path.find('.') == std::string::npos )
	{
		if( path.substr( 0, 6 ) != "setup/" && path != "setup" )
		{
			if( serveFromCacheOrDisk( "index.html" ) ) return;
		}
	}

	res.code = 404;
}

void CrowListener::ReadBuildHash()
{
	auto hashPath = fs::path( m_StaticRoot ) / "build-hash.txt";
	std::ifstream file( hashPath );
	if( file )
	{
		std::string hash;
		std::getline( file, hash );
		// Trim whitespace
		while( !hash.empty() && (hash.back() == '\r' || hash.back() == '\n' || hash.back() == ' ') )
			hash.pop_back();
		if( !hash.empty() )
		{
			if( !m_GlobalContext->BuildHash.empty() && m_GlobalContext->BuildHash != hash )
			{
				// Build hash changed — invalidate static file cache
				std::lock_guard<std::mutex> lock( m_FileCacheMutex );
				m_FileCache.clear();
				LOG_INFO( "Build hash changed (%s -> %s), static file cache cleared", m_GlobalContext->BuildHash.c_str(), hash.c_str() );
			}
			m_GlobalContext->BuildHash = hash;
		}
	}
	else
	{
		LOG_WARNING( "No build-hash.txt found — auto-refresh disabled" );
	}
}
