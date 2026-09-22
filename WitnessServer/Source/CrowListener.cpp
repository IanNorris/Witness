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
#include <cctype>

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
	void log( const std::string& message, crow::LogLevel level ) override
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

namespace
{
	struct MseStreamSelection
	{
		int CameraId = 0;
		int ChannelId = 0;
		bool SubStream = false;
		int Width = 0;
		int Height = 0;
		std::string Codec;
		std::shared_ptr<Witness::Camera::LiveOutputStream> LiveStream;
	};

	int BaseCameraId( int ChannelId )
	{
		return ChannelId >= StreamBroadcaster::SubStreamChannelOffset ?
			ChannelId - StreamBroadcaster::SubStreamChannelOffset : ChannelId;
	}

	int ParseViewportDimension( const char* Value )
	{
		if( !Value ) return 0;
		const long Parsed = std::strtol( Value, nullptr, 10 );
		return Parsed > 0 && Parsed <= 16384 ? static_cast<int>( Parsed ) : 0;
	}

	MseStreamSelection SelectMseStream(
		GlobalContext& Context, int CameraId, int RequestedWidth, int RequestedHeight,
		bool ForceSubStream = false )
	{
		MseStreamSelection Selection;
		Selection.CameraId = CameraId;
		Selection.ChannelId = CameraId;

		std::shared_ptr<CameraWorker> Worker;
		{
			std::shared_lock<std::shared_mutex> Lock( Context.Mutex );
			auto Camera = Context.GetCameraMap().find( CameraId );
			if( Camera == Context.GetCameraMap().end() ) return Selection;
			Worker = Camera->second.Worker;
		}
		if( !Worker ) return Selection;

		const auto SubWorker = Worker->GetSubStreamWorker();
		const auto SubLive = SubWorker ? SubWorker->GetLiveStream() : nullptr;
		const int SubWidth = SubWorker ? SubWorker->GetVideoWidth() : 0;
		const int SubHeight = SubWorker ? SubWorker->GetVideoHeight() : 0;
		const std::string SubCodec = SubWorker ? SubWorker->GetCodecName() : std::string{};
		const auto SubInit = SubLive ? SubLive->GetInitSnapshot() :
			Witness::Camera::LiveStreamInitSnapshot{};
		const bool SubReady = SubWorker && SubWorker->IsConnected() && SubLive &&
			SubWidth > 0 && SubHeight > 0 && !SubCodec.empty() &&
			SubInit.Data && !SubInit.Data->empty();
		bool UseSubStream = ForceSubStream && SubReady;
		if( !ForceSubStream && RequestedWidth > 0 && RequestedHeight > 0 && SubReady )
		{
			// Allow modest upscaling so small dashboard tiles stay on the cheaper stream.
			UseSubStream = RequestedWidth <= static_cast<int>( std::ceil( SubWidth * 1.25 ) ) &&
				RequestedHeight <= static_cast<int>( std::ceil( SubHeight * 1.25 ) );
		}

		if( UseSubStream )
		{
			Selection.SubStream = true;
			Selection.ChannelId = CameraId + StreamBroadcaster::SubStreamChannelOffset;
			Selection.Width = SubWidth;
			Selection.Height = SubHeight;
			Selection.Codec = SubCodec;
			Selection.LiveStream = SubLive;
		}
		else
		{
			Selection.Width = Worker->GetVideoWidth();
			Selection.Height = Worker->GetVideoHeight();
			Selection.Codec = Worker->GetVideoCodecName();
			Selection.LiveStream = Worker->GetLiveStream();
		}
		return Selection;
	}

	std::string BuildMseStreamSelection(
		const MseStreamSelection& Selection, int RequestedWidth, int RequestedHeight, bool Changed )
	{
		crow::json::wvalue Control;
		Control["type"] = "streamSelection";
		Control["stream"] = Selection.SubStream ? "sub" : "main";
		Control["codec"] = Selection.Codec;
		Control["width"] = Selection.Width;
		Control["height"] = Selection.Height;
		Control["requestedWidth"] = RequestedWidth;
		Control["requestedHeight"] = RequestedHeight;
		Control["changed"] = Changed;
		return Control.dump();
	}

	void SubscribeMseStream(
		GlobalContext& Context, crow::websocket::connection* Conn,
		const MseStreamSelection& Selection, int RequestedWidth, int RequestedHeight, bool Changed )
	{
		if( !Selection.LiveStream ) return;
		Context.Streams->SubscribeWithBootstrap(
			Selection.ChannelId, Conn,
			BuildMseStreamSelection( Selection, RequestedWidth, RequestedHeight, Changed ),
			[LiveStream = Selection.LiveStream]()
			{
				StreamBroadcaster::BootstrapPayload Payload;
				const auto Init = LiveStream->GetInitSnapshot();
				if( !Init.Data || Init.Data->empty() ) return Payload;
				crow::json::wvalue Control;
				Control["type"] = "initSegment";
				Control["generation"] = Init.Generation;
				if( !Init.AudioCodec.empty() ) Control["audioCodec"] = Init.AudioCodec;
				Payload.ControlJson = Control.dump();
				Payload.Data = Init.Data;
				return Payload;
			} );
	}
}

static void CaptureStreamDiagnosticAnomaly(
	GlobalContext& Context, crow::websocket::connection& Conn,
	const std::string& Data, bool IsBinary)
{
	if (IsBinary || Data.size() > 512)
		return;
	auto Body = crow::json::load(Data);
	if (!Body || !Body.has("type") || !Body.has("reason"))
		return;

	std::string Reason;
	try
	{
		if (std::string(Body["type"].s()) != "diagnosticAnomaly")
			return;
		Reason = std::string(Body["reason"].s());
	}
	catch (...)
	{
		return;
	}

	if (Reason.empty())
		return;
	Reason.resize((std::min)(Reason.size(), size_t{64}));
	for (char& Character : Reason)
	{
		const unsigned char Value = static_cast<unsigned char>(Character);
		if (!std::isalnum(Value) && Character != '-' && Character != '_')
			Character = '_';
	}

	const int InitialChannel = static_cast<int>(reinterpret_cast<intptr_t>(Conn.userdata()));
	const int CameraId = BaseCameraId( InitialChannel );
	const int CurrentChannel = Context.Streams->GetSubscriptionChannel( &Conn );
	const bool SubStream = CurrentChannel >= StreamBroadcaster::SubStreamChannelOffset;
	std::shared_ptr<Witness::Camera::LiveOutputStream> LiveStream;
	{
		std::shared_lock<std::shared_mutex> Lock(Context.Mutex);
		auto Camera = Context.GetCameraMap().find(CameraId);
		if (Camera == Context.GetCameraMap().end() || !Camera->second.Worker)
			return;
		LiveStream = SubStream ? Camera->second.Worker->GetSubStreamLive() :
			Camera->second.Worker->GetLiveStream();
	}
	if (LiveStream)
		LiveStream->CaptureDiagnosticAnomaly(Reason);
}

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

	size_t StaticFileCount = 0;
	ScanStaticFiles( StaticFileCount );
	LOG_INFO( "Static root: %s (%zu files)", m_StaticRoot.c_str(), StaticFileCount );

	// Read build hash for auto-refresh detection
	ReadBuildHash();

	RegisterRoutes();
}

bool CrowListener::ScanStaticFiles( size_t& FileCount )
{
	std::unordered_map<std::string, std::string> StaticFiles;
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
	fs::recursive_directory_iterator Entry( m_StaticRoot, ec );
	const fs::recursive_directory_iterator End;
	while( !ec && Entry != End )
	{
		const bool IsDirectory = Entry->is_directory( ec );
		if( ec ) break;
		if( !IsDirectory )
		{
			auto RelPath = fs::relative( Entry->path(), m_StaticRoot, ec );
			if( ec ) break;
			std::string PathStr = RelPath.generic_string();

			std::string ContentType = "application/octet-stream";
			if( Entry->path().has_extension() )
			{
				std::string Ext = Entry->path().extension().string().substr(1);
				auto It = MimeTypes.find( Ext );
				if( It != MimeTypes.end() )
				{
					ContentType = It->second;
				}
			}

			StaticFiles[PathStr] = ContentType;
		}
		Entry.increment( ec );
	}

	if( ec )
	{
		LOG_ERROR( "Static file scan error: %s", ec.message().c_str() );
		return false;
	}
	if( StaticFiles.find( "index.html" ) == StaticFiles.end() )
	{
		LOG_WARNING( "Static file scan incomplete: index.html not found" );
		return false;
	}

	FileCount = StaticFiles.size();
	{
		std::lock_guard<std::mutex> Lock( m_FileCacheMutex );
		m_StaticFiles = std::move( StaticFiles );
		m_FileCache.clear();
		m_StaticFilesGeneration++;
		m_StaticFilesReady = true;
	}
	return true;
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

	CROW_ROUTE( m_App, "/api/keys" )
	([this]( const crow::request& req, crow::response& res ) { HandleApiKeyList( req, res ); });
	CROW_ROUTE( m_App, "/api/keys/create" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res ) { HandleApiKeyCreate( req, res ); });
	CROW_ROUTE( m_App, "/api/keys/revoke" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res ) { HandleApiKeyRevoke( req, res ); });
	CROW_ROUTE( m_App, "/api/keys/audit" )
	([this]( const crow::request& req, crow::response& res ) { HandleApiKeyAudit( req, res ); });
	CROW_ROUTE( m_App, "/api/v1/diagnostics/health" )
	([this]( const crow::request& req, crow::response& res ) { HandleDebugHealth( req, res ); });
	CROW_ROUTE( m_App, "/api/v1/clips/search" )
	([this]( const crow::request& req, crow::response& res ) { HandleApiClipSearch( req, res ); });
	CROW_ROUTE( m_App, "/api/v1/cameras/<int>/record" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId ) { HandleApiCameraRecord( req, res, cameraId ); });

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

	CROW_ROUTE( m_App, "/clip/retag/bulk" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleClipRetagBulk( req, res );
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

	// Actions
	CROW_ROUTE( m_App, "/action/enum" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionEnum( req, res );
	});

	CROW_ROUTE( m_App, "/action/create" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionCreate( req, res );
	});

	CROW_ROUTE( m_App, "/action/update" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionUpdate( req, res );
	});

	CROW_ROUTE( m_App, "/action/delete" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionDelete( req, res );
	});

	CROW_ROUTE( m_App, "/action/assign" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionAssign( req, res );
	});

	CROW_ROUTE( m_App, "/action/unassign" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionUnassign( req, res );
	});

	CROW_ROUTE( m_App, "/action/sounds" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionSounds( req, res );
	});

	CROW_ROUTE( m_App, "/action/test_sound" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleActionTestSound( req, res );
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

	CROW_ROUTE( m_App, "/debug/packet-capture" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugPacketCapture( req, res );
	});

	CROW_ROUTE( m_App, "/debug/health" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleDebugHealth( req, res );
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

	CROW_ROUTE( m_App, "/api/reprocess/queue" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleReprocessQueue( req, res );
	});

	CROW_ROUTE( m_App, "/api/detection/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandleDetectionQuery( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/api/trails/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandleTrailsQuery( req, res, cameraId );
	});

	// Face detection / crops
	CROW_ROUTE( m_App, "/api/faces/<int>" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandleFaceQuery( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/api/faces/recent" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleFaceRecent( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/crop/<int>" )
	([this]( const crow::request& req, crow::response& res, int cropUID )
	{
		HandleFaceCropImage( req, res, cropUID );
	});

	CROW_ROUTE( m_App, "/api/detection/crop/<path>" )
	([this]( const crow::request& req, crow::response& res, const std::string& cropPath )
	{
		HandleDetectionCropImage( req, res, cropPath );
	});

	CROW_ROUTE( m_App, "/api/detection/frame/<int>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& filename )
	{
		HandleDetectionFrameImage( req, res, cameraId, filename );
	});

	// Face recognition
	CROW_ROUTE( m_App, "/api/face/known" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleKnownFaceList( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/known/create" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleKnownFaceCreate( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/known/update" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleKnownFaceUpdate( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/known/delete" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleKnownFaceDelete( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/assign" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleFaceAssign( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/unassign" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleFaceUnassign( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/merge" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleFaceMerge( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/unknown" )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleUnidentifiedFaces( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/sightings/<int>" )
	([this]( const crow::request& req, crow::response& res, int knownFaceId )
	{
		HandleFaceSightings( req, res, knownFaceId );
	});

	CROW_ROUTE( m_App, "/api/face/reprocess" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleFaceReprocess( req, res );
	});

	CROW_ROUTE( m_App, "/api/face/upload" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleFaceUpload( req, res );
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

	CROW_ROUTE( m_App, "/dvr/events/<int>/<string>/<string>" )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& from, const std::string& to )
	{
		HandleDvrEvents( req, res, cameraId, from, to );
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

	// PTZ control
	CROW_ROUTE( m_App, "/ptz/<int>/<string>" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId, const std::string& command )
	{
		HandlePtzCommand( req, res, cameraId, command );
	});

	CROW_ROUTE( m_App, "/ptz/<int>/position" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePtzPosition( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/ptz/<int>/zoom" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePtzZoomGet( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/ptz/<int>/zoom/set" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePtzZoomSet( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/ptz/<int>/presets" )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePtzPresets( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/ptz/<int>/preset/set" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePtzPresetSet( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/ptz/<int>/preset/delete" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res, int cameraId )
	{
		HandlePtzPresetDelete( req, res, cameraId );
	});

	CROW_ROUTE( m_App, "/ptz/test" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandlePtzTest( req, res );
	});

	// WebSocket MSE stream
	CROW_WEBSOCKET_ROUTE( m_App, "/ws/stream/<int>" )
		.onaccept([this]( const crow::request& req, void** userdata ) -> bool
		{
			// Extract camera ID from URL (last path segment)
			std::string url = req.url;
			auto lastSlash = url.rfind('/');
			if( lastSlash == std::string::npos ) return false;
			int cameraId = std::atoi( url.substr( lastSlash + 1 ).c_str() );
			if( !CrowAuth::CanAccessStream( *m_GlobalContext, req, cameraId ) ) return false;

			const int requestedWidth = ParseViewportDimension( req.url_params.get( "width" ) );
			const int requestedHeight = ParseViewportDimension( req.url_params.get( "height" ) );
			const auto selection = SelectMseStream(
				*m_GlobalContext, cameraId, requestedWidth, requestedHeight );
			if( !selection.LiveStream )
				return false;

			// Store the initial channel. Both channel forms can always recover the base camera ID.
			*userdata = reinterpret_cast<void*>( static_cast<intptr_t>( selection.ChannelId ) );
			return true;
		})
		.onopen([this]( crow::websocket::connection& conn )
		{
			const int initialChannel = static_cast<int>( reinterpret_cast<intptr_t>( conn.userdata() ) );
			const int cameraId = BaseCameraId( initialChannel );
			const bool useSubStream = initialChannel >= StreamBroadcaster::SubStreamChannelOffset;
			const auto selection = SelectMseStream( *m_GlobalContext, cameraId, 0, 0, useSubStream );

			SubscribeMseStream( *m_GlobalContext, &conn, selection, 0, 0, false );

			LOG_INFO( "[MSE] Stream client connected for camera %d using %s stream",
				cameraId, selection.SubStream ? "sub" : "main" );
		})
		.onclose([this]( crow::websocket::connection& conn, const std::string& /*reason*/, uint16_t /*statusCode*/ )
		{
			m_GlobalContext->Streams->Unsubscribe( &conn );
		})
		.onmessage([this]( crow::websocket::connection& conn, const std::string& data, bool is_binary )
		{
			if( !is_binary && data.size() <= 512 )
			{
				try
				{
					auto body = crow::json::load( data );
					if( body && body.has( "type" ) && std::string( body["type"].s() ) == "viewport" &&
						body.has( "width" ) && body.has( "height" ) )
					{
						const int64_t widthValue = body["width"].i();
						const int64_t heightValue = body["height"].i();
						if( widthValue <= 0 || widthValue > 16384 ||
							heightValue <= 0 || heightValue > 16384 )
							return;
						const int requestedWidth = static_cast<int>( widthValue );
						const int requestedHeight = static_cast<int>( heightValue );
						const int initialChannel = static_cast<int>( reinterpret_cast<intptr_t>( conn.userdata() ) );
						const int cameraId = BaseCameraId( initialChannel );
						const auto selection = SelectMseStream(
							*m_GlobalContext, cameraId, requestedWidth, requestedHeight );
						const int currentChannel = m_GlobalContext->Streams->GetSubscriptionChannel( &conn );
						if( selection.LiveStream && selection.ChannelId != currentChannel )
						{
							// Freeze and tear down the old decoder before any data from the new
							// channel can arrive. New partials are ignored until its init/keyframe.
							SubscribeMseStream( *m_GlobalContext, &conn, selection,
								requestedWidth, requestedHeight, true );
							LOG_INFO( "[MSE] Camera %d switched to %s stream for viewport %dx%d",
								cameraId, selection.SubStream ? "sub" : "main",
								requestedWidth, requestedHeight );
						}
						return;
					}
				}
				catch( ... ) { return; }
			}
			CaptureStreamDiagnosticAnomaly(*m_GlobalContext, conn, data, is_binary);
		});

	// WebSocket sub-stream (H.264 fallback for clients that can't decode the main stream)
	CROW_WEBSOCKET_ROUTE( m_App, "/ws/stream/sub/<int>" )
		.onaccept([this]( const crow::request& req, void** userdata ) -> bool
		{
			std::string url = req.url;
			auto lastSlash = url.rfind('/');
			if( lastSlash == std::string::npos ) return false;
			int cameraId = std::atoi( url.substr( lastSlash + 1 ).c_str() );
			if( !CrowAuth::CanAccessStream( *m_GlobalContext, req, cameraId ) ) return false;

			const auto selection = SelectMseStream( *m_GlobalContext, cameraId, 0, 0, true );
			if( !selection.SubStream )
				return false;

			*userdata = reinterpret_cast<void*>( static_cast<intptr_t>(
				cameraId + StreamBroadcaster::SubStreamChannelOffset ) );
			return true;
		})
		.onopen([this]( crow::websocket::connection& conn )
		{
			int subChannelId = static_cast<int>( reinterpret_cast<intptr_t>( conn.userdata() ) );
			int cameraId = BaseCameraId( subChannelId );

			const auto selection = SelectMseStream( *m_GlobalContext, cameraId, 0, 0, true );
			if( !selection.SubStream )
			{
				conn.close( "sub-stream unavailable" );
				return;
			}
			SubscribeMseStream( *m_GlobalContext, &conn, selection, 0, 0, false );

			LOG_INFO( "[MSE] Sub-stream client connected for camera %d", cameraId );
		})
		.onclose([this]( crow::websocket::connection& conn, const std::string& /*reason*/, uint16_t /*statusCode*/ )
		{
			m_GlobalContext->Streams->Unsubscribe( &conn );
		})
		.onmessage([this]( crow::websocket::connection& conn, const std::string& data, bool is_binary )
		{
			CaptureStreamDiagnosticAnomaly(*m_GlobalContext, conn, data, is_binary);
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
					cam["motionActive"] = state.IsMotionActive;
					cams.push_back( std::move( cam ) );
				}
			}
			initData["cameras"] = std::move( cams );
			{
				std::shared_lock<std::shared_mutex> lock( m_GlobalContext->Mutex );
				if( !m_GlobalContext->BuildHash.empty() )
					initData["buildHash"] = m_GlobalContext->BuildHash;
			}

			crow::json::wvalue envelope;
			envelope["event"] = "init";
			envelope["data"] = std::move( initData );
			conn.send_text( envelope.dump() );
		})
		.onclose([this]( crow::websocket::connection& conn, const std::string& /*reason*/, uint16_t /*statusCode*/ )
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

	// Static file routes -- serve Vue SPA
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
		auto SetHeaders = [&]( const std::string& ContentType )
		{
			res.set_header( "Content-Type", ContentType );
			res.set_header( "Cache-Control", key == "index.html" || key == "build-hash.txt" ?
				"no-cache, no-store, must-revalidate" :
				( key.rfind( "assets/", 0 ) == 0 ? "public, max-age=31536000, immutable" : "no-cache" ) );
		};

		// A deployment may replace the Web directory while a request is reading.
		// Retry once if the inventory changes, and never repopulate a refreshed
		// cache with content read under the previous generation.
		for( int Attempt = 0; Attempt < 2; Attempt++ )
		{
			std::string ContentType;
			uint64_t Generation = 0;
			{
				std::lock_guard<std::mutex> Lock( m_FileCacheMutex );
				auto It = m_StaticFiles.find( key );
				if( It == m_StaticFiles.end() ) return false;
				ContentType = It->second;
				Generation = m_StaticFilesGeneration;

				auto CacheIt = m_FileCache.find( key );
				if( CacheIt != m_FileCache.end() )
				{
					SetHeaders( ContentType );
					res.body = CacheIt->second;
					res.code = 200;
					return true;
				}
			}

			fs::path FullPath = fs::path( m_StaticRoot ) / key;
			std::ifstream File( FullPath, std::ios::binary );
			if( !File ) return false;
			std::string Body( (std::istreambuf_iterator<char>(File)),
				std::istreambuf_iterator<char>() );

			{
				std::lock_guard<std::mutex> Lock( m_FileCacheMutex );
				if( Generation != m_StaticFilesGeneration ) continue;
				m_FileCache[key] = Body;
			}

			SetHeaders( ContentType );
			res.body = std::move( Body );
			res.code = 200;
			return true;
		}
		return false;
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
			std::string PreviousHash;
			{
				std::shared_lock<std::shared_mutex> Lock( m_GlobalContext->Mutex );
				PreviousHash = m_GlobalContext->BuildHash;
			}
			bool StaticFilesReady = false;
			{
				std::lock_guard<std::mutex> Lock( m_FileCacheMutex );
				StaticFilesReady = m_StaticFilesReady;
			}
			if( !StaticFilesReady || PreviousHash != hash )
			{
				// Hashed asset names change on each build, so refresh the inventory as
				// well as cached file contents before asking clients to reload.
				size_t StaticFileCount = 0;
				if( !ScanStaticFiles( StaticFileCount ) ) return;
				LOG_INFO( "Build hash changed (%s -> %s), static files refreshed (%zu files)",
					PreviousHash.c_str(), hash.c_str(), StaticFileCount );
			}
			{
				std::unique_lock<std::shared_mutex> Lock( m_GlobalContext->Mutex );
				m_GlobalContext->BuildHash = hash;
			}
		}
	}
	else
	{
		LOG_WARNING( "No build-hash.txt found -- auto-refresh disabled" );
	}
}
