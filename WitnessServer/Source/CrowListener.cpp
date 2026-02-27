#include "CrowListener.h"
#include "CrowAuth.h"
#include "CameraWorker.h"
#include "Messages.h"
#include "AuthHelpers.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <format>
#include <cmath>

#include "sodium.h"

namespace fs = std::filesystem;

CrowListener::CrowListener( const std::string& Hostname, int Port, bool Secure, DebugConsole* DebugConsoleInstance )
: m_DebugConsole( DebugConsoleInstance )
, m_Hostname( Hostname )
, m_Port( Port )
, m_Secure( Secure )
{
	m_GlobalContext = std::make_unique<GlobalContext>();
	m_GlobalContext->Port = Port;

	std::string Scheme = Secure ? "https" : "http";
	m_BaseUri = Scheme + "://" + Hostname + ":" + std::to_string( Port );
}

CrowListener::~CrowListener()
{
	Stop();
}

void CrowListener::Initialise( const std::unordered_map< StringT, StringT >& Settings )
{
	// Load static file root
	StringT Errors;
	StringT Root;
	GetSettingsField( Settings, _T("server_root"), Root, Errors );
	m_StaticRoot = std::string( Root.begin(), Root.end() );

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
		std::cerr << "Static file scan error: " << ec.message() << std::endl;
	}

	std::cout << "Static root: " << m_StaticRoot << " (" << m_StaticFiles.size() << " files)" << std::endl;

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

	// Catch-all route for static files (lowest priority)
	CROW_CATCHALL_ROUTE( m_App )
	([this]( crow::response& res )
	{
		res.code = 404;
	});

	// Static file routes
	CROW_ROUTE( m_App, "/" )
	([this]( const crow::request& req, crow::response& res )
	{
		ServeStaticFile( req, res, "" );
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

	for( int pass = 0; pass < 2; pass++ )
	{
		auto it = m_StaticFiles.find( lookup );
		if( it != m_StaticFiles.end() )
		{
			fs::path fullPath = m_StaticRoot;
			fullPath /= it->first;

			std::ifstream file( fullPath, std::ios::binary );
			if( file )
			{
				std::string body( (std::istreambuf_iterator<char>(file)),
								  std::istreambuf_iterator<char>() );

				res.set_header( "Content-Type", it->second );
				res.set_header( "Content-Security-Policy",
					"default-src 'self'; "
					"script-src 'self' 'unsafe-inline' 'unsafe-eval' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
					"worker-src 'self' blob:; "
					"style-src 'self' 'unsafe-inline' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
					"script-src-elem 'self' 'unsafe-inline' 'unsafe-eval' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
					"style-src-attr 'self' 'unsafe-inline' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
					"img-src 'self' data: blob: https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; "
					"font-src 'self' data:; "
					"media-src 'self' blob:;" );

				res.body = std::move( body );
				res.code = 200;
				return;
			}
		}

		if( pass == 0 )
		{
			lookup += "/index.html";
		}
	}

	res.code = 404;
}
