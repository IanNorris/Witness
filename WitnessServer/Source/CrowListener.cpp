#include "CrowListener.h"
#include "CrowAuth.h"
#include "CameraWorker.h"
#include "Messages.h"
#include "Commands/Authenticate.h"

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

	StringT Scheme = Secure ? _T("https") : _T("http");
	m_BaseUri = Scheme + _T("://") + StringT( Hostname.begin(), Hostname.end() ) + _T(":") + std::to_wstring( Port );
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

	CROW_ROUTE( m_App, "/auth/getProfile" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthGetProfile( req, res );
	});

	CROW_ROUTE( m_App, "/auth/enumUsers" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthEnumUsers( req, res );
	});

	CROW_ROUTE( m_App, "/auth/newUser" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthNewUser( req, res );
	});

	CROW_ROUTE( m_App, "/auth/changePassword" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthChangePassword( req, res );
	});

	CROW_ROUTE( m_App, "/auth/toggleEnabled" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthToggleEnabled( req, res );
	});

	CROW_ROUTE( m_App, "/auth/toggleAdmin" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthToggleAdmin( req, res );
	});

	CROW_ROUTE( m_App, "/auth/setDisplayName" ).methods( crow::HTTPMethod::POST )
	([this]( const crow::request& req, crow::response& res )
	{
		HandleAuthSetDisplayName( req, res );
	});

	CROW_ROUTE( m_App, "/auth/setUserGroups" ).methods( crow::HTTPMethod::POST )
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
	([this]( const crow::request& req, crow::response& res )
	{
		std::string path = req.url;

		// Strip leading slash
		if( !path.empty() && path[0] == '/' )
			path = path.substr(1);

		ServeStaticFile( req, res, path );
	});
}

void CrowListener::ServeStaticFile( const crow::request& req, crow::response& res, const std::string& path )
{
	std::string lookup = path.empty() ? "index.html" : path;

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
				res.end();
				return;
			}
		}

		if( pass == 0 )
		{
			lookup += "/index.html";
		}
	}

	res.code = 404;
	res.end();
}

void CrowListener::HandlePreview( const crow::request& req, crow::response& res, int cameraId, bool largePreview )
{
	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	auto Camera = m_GlobalContext->FindCameraById( cameraId );
	if( Camera )
	{
		if( largePreview )
		{
			Camera->LastLargePreviewTimestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		}
		else
		{
			Camera->LastSmallPreviewTimestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		}

		auto PreviewRequest = std::make_shared<CameraPreviewRequestMessage>();
		PreviewRequest->LastLargePreviewTimestamp = Camera->LastLargePreviewTimestamp;
		PreviewRequest->LastSmallPreviewTimestamp = Camera->LastSmallPreviewTimestamp;
		m_GlobalContext->MessageBus->SendToClient( Camera->Worker.get(), PreviewRequest );

		res.set_header( "Content-Type", "image/jpeg" );
		res.set_header( "Cache-Control", "no-cache, no-store, must-revalidate" );
		res.body.assign( (const char*)Camera->PreviewThumbnail.data(), Camera->PreviewThumbnail.size() );
		res.code = 200;
		res.end();
	}
	else
	{
		res.code = 404;
		res.end();
	}
}

void CrowListener::HandleCameraEnum( const crow::request& req, crow::response& res, bool asAdmin, bool longPoll )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, asAdmin ? CrowAuth::Privilege::Administrator : CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	bool First = true;
	std::vector<int> State;
	std::vector<int> OriginalState;
	std::vector<crow::json::wvalue> Array;

	bool IsAcceptable = false;
	do {
		Array.clear();
		State.clear();

		{
			SQLiteDatabaseQueryInstance GetCamerasForUser( m_GlobalContext->Database, asAdmin ? _T("GetCameras") : _T("GetCamerasForUser") );
			GetCamerasForUser->Bind( "@User", UserUID );

			GetCamerasForUser->Execute(
				[&]( const SQLiteDatabaseQuery& query )
				{
					int ID = query.GetColumnValueInt( 0 );
					StringT Name = query.GetColumnValueText( 1 );
					StringT ConnectionString = query.GetColumnValueText( 2 );
					StringT ConnectionStringSub = query.GetColumnValueText( 3 );
					StringT Description = query.GetColumnValueText( 4 ) ? query.GetColumnValueText( 4 ) : _T("");
					int Enabled = query.GetColumnValueInt( 5 );

					if( Enabled || asAdmin )
					{
						crow::json::wvalue Camera;
						Camera["id"] = ID;
						Camera["name"] = StringToAnsi( Name );
						Camera["description"] = StringToAnsi( Description );
						Camera["enabled"] = Enabled;

						std::vector<crow::json::wvalue> Groups;

						SQLiteDatabaseQueryInstance SelectGroupsForCamera( m_GlobalContext->Database, _T("SelectGroupsForCamera") );
						SelectGroupsForCamera->Bind( "@Camera", ID );

						SelectGroupsForCamera->Execute(
							[&Groups]( const SQLiteDatabaseQuery& query )
							{
								int GroupId = query.GetColumnValueInt( 1 );
								Groups.push_back( GroupId );
								return true;
							}
						);

						if( asAdmin )
						{
							Camera["connectionString"] = StringToAnsi( ConnectionString );
							Camera["connectionStringSub"] = StringToAnsi( ConnectionStringSub );
						}

						Camera["groups"] = std::move( Groups );

						{
							auto CameraState = m_GlobalContext->FindCameraById( ID );
							if( CameraState )
							{
								if( longPoll )
								{
									State.push_back( ID );
									State.push_back( CameraState->IsRecording );
								}

								Camera["status"] = StringToAnsi( CameraState->Status );
								Camera["recording"] = CameraState->IsRecording;

								auto StreamStats = CameraState->Worker->GetStreamStats();

								auto ImgStats = m_GlobalContext->CommonImageProcessingJobQueue->GetStats( ID );
								Camera["lastTimestamp"] = ImgStats.LastTimestamp;

								if( asAdmin && ImgStats.FrameCount > 0 )
								{
									Camera["frameCount"] = ImgStats.FrameCount;

#define GET_STAT_CROW(OutputPrefix, StatName) \
	Camera[ OutputPrefix "TimeOfEachMS" ] = (double)ImgStats.Stats.FrameCount[StatName] ? ((double)ImgStats.Stats.Stats[StatName] / ((double)ImgStats.Stats.FrameCount[StatName] * 1000.0 * 1000.0)) : 0.0;\
	Camera[ OutputPrefix "ActualMS" ] = ImgStats.FrameCount ? (double)ImgStats.Stats.Stats[StatName] / ((double)ImgStats.FrameCount * 1000.0 * 1000.0) : 0

									GET_STAT_CROW("processing", FilterStat_Process_Total);
									GET_STAT_CROW("scale", FilterStat_Scale);
									GET_STAT_CROW("jpegEncoding", FilterStat_JpegEncoding);
									GET_STAT_CROW("observer", FilterStat_ObserverFilter);
									GET_STAT_CROW("firstPassFilter", FilterStat_FirstPassFilter);
									GET_STAT_CROW("secondPassFilter", FilterStat_SecondPassFilter);
									GET_STAT_CROW("thirdPassFilter", FilterStat_ThirdPassFilter);
									GET_STAT_CROW("debug", FilterStat_Debug);

									GET_STAT_CROW("mvfInternal", FilterStat_MVF_Internal);
									GET_STAT_CROW("mvfSideData", FilterStat_MVF_SideData);
									GET_STAT_CROW("mvfVectorPass", FilterStat_MVF_VectorPass);
									GET_STAT_CROW("mvfClusterPass", FilterStat_MVF_ClusterPass);
									GET_STAT_CROW("mvfObjectPass", FilterStat_MVF_ObjectPass);

#undef GET_STAT_CROW
								}

								if( asAdmin && StreamStats.FrameCount > 0 )
								{
									double Decode = (double)StreamStats.DecoderTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);
									double Output = (double)StreamStats.OutputTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);
									double Read = (double)StreamStats.ReadTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);

									Camera["streamReadTimeMS"] = Read;
									Camera["streamDecodeTimeMS"] = Decode;
									Camera["streamOutputTimeMS"] = Output;
								}
							}
						}

						Array.push_back( std::move( Camera ) );
					}

					return true;
				}
			);
		}

		if( longPoll )
		{
			if( First )
			{
				OriginalState = State;
				First = false;
			}
			else
			{
				if( OriginalState.size() != State.size() )
				{
					IsAcceptable = true;
				}
				else
				{
					bool Same = true;
					for( size_t Index = 0; Index < OriginalState.size(); Index++ )
					{
						if( OriginalState[Index] != State[Index] )
						{
							Same = false;
							IsAcceptable = true;
							break;
						}
					}

					if( !IsAcceptable )
					{
						LongPollScope Scope( m_GlobalContext->LongPoll );
						Scope.Wait();
					}
				}
			}
		}
		else
		{
			IsAcceptable = true;
		}
	} while( !IsAcceptable );

	crow::json::wvalue Result = std::move( Array );
	res.set_header( "Content-Type", "application/json" );
	res.body = Result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraRecord( const crow::request& req, crow::response& res, int cameraId )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	if( !body.has( "record" ) )
	{
		res.code = 400;
		res.body = "Missing 'record' field";
		res.end();
		return;
	}

	bool Record = body["record"].b();

	auto CameraState = m_GlobalContext->FindCameraById( cameraId );
	if( CameraState )
	{
		auto ToggleRecord = std::make_shared<CameraStateToggleRecordMessage>( cameraId, Record );
		m_GlobalContext->MessageBus->SendToClient( nullptr, ToggleRecord );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "\"OK\"";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraCreate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	std::string ConnectionString = body.has("connectionString") ? std::string(body["connectionString"].s()) : "";
	std::string ConnectionStringSub = body.has("connectionStringSub") ? std::string(body["connectionStringSub"].s()) : "";

	StringT DisplayNameW( DisplayName.begin(), DisplayName.end() );
	StringT DescriptionW( Description.begin(), Description.end() );
	StringT ConnectionStringW( ConnectionString.begin(), ConnectionString.end() );
	StringT ConnectionStringSubW( ConnectionStringSub.begin(), ConnectionStringSub.end() );

	SQLiteDatabaseQueryInstance CreateCamera( m_GlobalContext->Database, _T("CreateCamera") );
	CreateCamera->Bind( "@CameraName", DisplayNameW.c_str() );
	CreateCamera->Bind( "@Description", DescriptionW.c_str() );
	CreateCamera->Bind( "@CameraString", ConnectionStringW.c_str() );
	CreateCamera->Bind( "@CameraStringSub", ConnectionStringSubW.c_str() );

	if( CreateCamera->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } ) < 0 )
	{
		crow::json::wvalue Data;
		Data["errorMessage"] = StringToAnsi( CreateCamera->GetLastError() );
		res.set_header( "Content-Type", "application/json" );
		res.body = Data.dump();
		res.code = 400;
		res.end();
		return;
	}

	int64_t RowResult = CreateCamera->GetLastInsertionId();

	auto AddMessage = std::make_shared<CameraAddedMessage>( (int)RowResult );
	m_GlobalContext->MessageBus->SendToClient( nullptr, AddMessage );

	res.set_header( "Content-Type", "application/json" );
	res.body = "\"OK\"";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("id") )
	{
		res.code = 400;
		res.body = "Missing 'id' field";
		res.end();
		return;
	}

	int CameraUID = (int)body["id"].i();

	{
		SQLiteDatabaseQueryInstance DeleteCamera( m_GlobalContext->Database, _T("DeleteCamera") );
		DeleteCamera->Bind( "@CameraId", CameraUID );
		DeleteCamera->Execute( nullptr );
	}

	auto DeleteMsg = std::make_shared<CameraRemovedMessage>( CameraUID );
	m_GlobalContext->MessageBus->SendToClient( nullptr, DeleteMsg );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraSetGroups( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("camera") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing 'camera' or 'value' field";
		res.end();
		return;
	}

	int CameraID = (int)body["camera"].i();

	std::vector<int> CameraGroupsRequested;
	for( auto& Element : body["value"] )
	{
		CameraGroupsRequested.push_back( (int)Element.i() );
	}

	std::vector<int> CameraGroupsCurrent;

	SQLiteDatabaseQueryInstance SelectGroupsForCamera( m_GlobalContext->Database, _T("SelectGroupsForCamera") );
	SelectGroupsForCamera->Bind( "@Camera", CameraID );

	SelectGroupsForCamera->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			int Group = query.GetColumnValueInt( 1 );
			CameraGroupsCurrent.push_back( Group );
			return true;
		}
	);

	// Add new groups
	for( int Value : CameraGroupsRequested )
	{
		if( find( CameraGroupsCurrent.begin(), CameraGroupsCurrent.end(), Value ) == CameraGroupsCurrent.end() )
		{
			SQLiteDatabaseQueryInstance CreateMapping( m_GlobalContext->Database, _T("CreateCameraGroupMapping") );
			CreateMapping->Bind( "@Camera", CameraID );
			CreateMapping->Bind( "@Group", Value );
			CreateMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	// Remove old groups
	for( int Value : CameraGroupsCurrent )
	{
		if( find( CameraGroupsRequested.begin(), CameraGroupsRequested.end(), Value ) == CameraGroupsRequested.end() )
		{
			SQLiteDatabaseQueryInstance DeleteMapping( m_GlobalContext->Database, _T("DeleteCameraGroupMapping") );
			DeleteMapping->Bind( "@Camera", CameraID );
			DeleteMapping->Bind( "@Group", Value );
			DeleteMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	m_GlobalContext->LongPoll->NotifyAll();

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraResetStats( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	{
		std::lock_guard<std::mutex> Lock( m_GlobalContext->Mutex );

		for( auto Camera : m_GlobalContext->GetCameraMap() )
		{
			m_GlobalContext->CommonImageProcessingJobQueue->ResetStats( Camera.first );
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "\"OK\"";
	res.code = 200;
	res.end();
}

void CrowListener::HandlePlaylist( const crow::request& req, crow::response& res, int cameraId )
{
	auto CameraState = m_GlobalContext->FindCameraById( cameraId );
	if( !CameraState )
	{
		res.code = 404;
		res.end();
		return;
	}

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();
	if( !LiveStream )
	{
		res.code = 503;
		res.end();
		return;
	}

	std::vector<LiveStreamSegment> Segments;
	LiveStream->GetSegments( Segments );

	int CurrentSegment = LiveStream->GetCurrentSegment();
	int InitGeneration = LiveStream->GetInitGeneration();

	size_t bufferSegments = Segments.size();

	std::ostringstream Playlist;
	Playlist << "#EXTM3U\n";
	Playlist << "#EXT-X-VERSION:7\n";

	// Check for _HLS_msn query param
	auto msnParam = req.url_params.get( "_HLS_msn" );
	uint64_t msnStart = msnParam ? std::stoull( msnParam ) : 0;

	int startAtSegment = 0;
	if( bufferSegments > 0 )
	{
		double MaxLength = 0.0;
		for( int segment = 0; segment < (int)bufferSegments; segment++ )
		{
			LiveStreamSegment& Seg = Segments[segment];
			if( Seg.Ready )
			{
				if( Seg.Duration > MaxLength )
					MaxLength = Seg.Duration;
				if( (uint64_t)Seg.SegmentIndex == msnStart )
				{
					MaxLength = Seg.Duration;
					startAtSegment = segment;
				}
			}
		}

		double PartialTarget = LiveStream->GetPartialTargetDuration();
		double HoldbackLength = PartialTarget * 3.0;
		Playlist << "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=NO,PART-HOLD-BACK=" << HoldbackLength << "\n";
		Playlist << "#EXT-X-PART-INF:PART-TARGET=" << PartialTarget << "\n";

		double TargetDuration = MaxLength * 1.18;
		if( TargetDuration < 1.0 ) TargetDuration = 1.0;

		Playlist << "#EXT-X-MEDIA-SEQUENCE:" << Segments[startAtSegment].SegmentIndex << "\n";
		Playlist << "#EXT-X-INDEPENDENT-SEGMENTS\n";
		Playlist << "#EXT-X-TARGETDURATION:" << (int)std::ceil( TargetDuration ) << "\n";
		Playlist << "#EXT-X-MAP:URI=\"" << cameraId << "/0/i?g=" << InitGeneration << "\"\n";
		Playlist << "\n";

		Playlist.precision(4);

		for( int segment = startAtSegment; segment < (int)bufferSegments; segment++ )
		{
			LiveStreamSegment& Seg = Segments[segment];
			if( Seg.Ready )
			{
				if( Seg.Discontinuity )
				{
					Playlist << "#EXT-X-DISCONTINUITY\n";
					Playlist << "#EXT-X-MAP:URI=\"" << cameraId << "/0/i?g=" << InitGeneration << "\"\n";
				}

				std::string dateTimeFormat = std::format( "{:%Y-%m-%dT%H:%M:%S}", Seg.SegmentTime );
				Playlist << "#EXT-X-PROGRAM-DATE-TIME:" << dateTimeFormat << "\n";

				for( auto& Partial : Seg.Partials )
				{
					Playlist << "#EXT-X-PART:DURATION=" << Partial.Duration
						<< ",URI=\"" << cameraId << "/" << Seg.SegmentIndex << "/" << Partial.PartIndex << "\"";
					if( Partial.Independent )
						Playlist << ",INDEPENDENT=YES";
					Playlist << "\n";
				}

				Playlist << "#EXTINF:" << Seg.Duration << ",\n";
				Playlist << cameraId << "/" << Seg.SegmentIndex << "/f\n";
			}
			else
			{
				if( Seg.Discontinuity )
				{
					Playlist << "#EXT-X-DISCONTINUITY\n";
					Playlist << "#EXT-X-MAP:URI=\"" << cameraId << "/0/i?g=" << InitGeneration << "\"\n";
				}

				for( auto& Partial : Seg.Partials )
				{
					Playlist << "#EXT-X-PART:DURATION=" << Partial.Duration
						<< ",URI=\"" << cameraId << "/" << Seg.SegmentIndex << "/" << Partial.PartIndex << "\"";
					if( Partial.Independent )
						Playlist << ",INDEPENDENT=YES";
					Playlist << "\n";
				}
			}
		}
	}

	res.set_header( "Content-Type", "application/vnd.apple.mpegurl" );
	res.set_header( "Cache-Control", "no-cache, no-store, must-revalidate" );
	res.body = Playlist.str();
	res.code = 200;
	res.end();
}

void CrowListener::HandleSegment( const crow::request& req, crow::response& res, int cameraId, int segmentId, const std::string& partId )
{
	auto CameraState = m_GlobalContext->FindCameraById( cameraId );
	if( !CameraState )
	{
		res.code = 404;
		res.end();
		return;
	}

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();
	if( !LiveStream )
	{
		res.code = 503;
		res.end();
		return;
	}

	bool IsFull = !partId.empty() && partId[0] == 'f';
	bool IsInit = !partId.empty() && partId[0] == 'i';
	bool IsPartial = !partId.empty() && partId[0] >= '0' && partId[0] <= '9';

	if( IsInit )
	{
		SegmentBuffer InitData = LiveStream->GetInitSegment();
		if( InitData && !InitData->empty() )
		{
			res.set_header( "Content-Type", "video/mp4" );
			res.set_header( "Access-Control-Allow-Origin", "*" );
			res.body.assign( (const char*)InitData->data(), InitData->size() );
			res.code = 200;
			res.end();
			return;
		}
	}
	else if( IsFull )
	{
		std::vector<LiveStreamSegment> Segments;
		LiveStream->GetSegments( Segments );

		for( auto& Seg : Segments )
		{
			if( Seg.SegmentIndex == segmentId && Seg.Ready && Seg.Data && !Seg.Data->empty() )
			{
				res.set_header( "Content-Type", "video/mp4" );
				res.set_header( "Access-Control-Allow-Origin", "*" );
				res.body.assign( (const char*)Seg.Data->data(), Seg.Data->size() );
				res.code = 200;
				res.end();
				return;
			}
		}
	}
	else if( IsPartial )
	{
		int targetPart = std::atoi( partId.c_str() );

		std::vector<LiveStreamSegment> Segments;
		LiveStream->GetSegments( Segments );

		for( auto& Seg : Segments )
		{
			if( Seg.SegmentIndex == segmentId )
			{
				if( targetPart >= 0 && targetPart < (int)Seg.Partials.size() )
				{
					auto& Partial = Seg.Partials[targetPart];
					if( Partial.Data && !Partial.Data->empty() )
					{
						res.set_header( "Content-Type", "video/mp4" );
						res.set_header( "Access-Control-Allow-Origin", "*" );
						res.body.assign( (const char*)Partial.Data->data(), Partial.Data->size() );
						res.code = 200;
						res.end();
						return;
					}
				}
				break;
			}
		}
	}

	res.code = 404;
	res.end();
}

// ===== Auth Handlers =====

void CrowListener::HandleAuthLogin( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("username") || !body.has("password") )
	{
		res.code = 400;
		res.body = "Missing username or password";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	std::string PasswordStr = body["password"].s();

	StringT Username( UsernameStr.begin(), UsernameStr.end() );
	StringT Password( PasswordStr.begin(), PasswordStr.end() );

	std::transform( Username.begin(), Username.end(), Username.begin(), ::tolower );

	int UserUID = -1;

	{
		SQLiteDatabaseQueryInstance FindUser( m_GlobalContext->Database, _T("FindUser") );
		FindUser->Bind( "@Username", Username.c_str() );

		int PasswordAlgorithm = 0;
		StringT PasswordHash;

		bool Success = false;
		FindUser->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				UserUID = query.GetColumnValueInt( 0 );
				PasswordHash = query.GetColumnValueText( 2 );
				PasswordAlgorithm = query.GetColumnValueInt( 3 );
				Success = true;
				return true;
			}
		);

		if( Success )
		{
			bool Result = false;
			switch( PasswordAlgorithm )
			{
			case 0:
				Result = CheckHashedPasswordKey_Algorithm0( PasswordHash, Username, Password );
				break;
			default:
				res.code = 401;
				res.body = "Unsupported password hash algorithm";
				res.end();
				return;
			}

			if( !Result )
			{
				res.code = 401;
				res.end();
				return;
			}
		}
		else
		{
			res.code = 401;
			res.end();
			return;
		}
	}

	int Enabled = 0;
	int Admin = 0;

	{
		bool Success = false;
		SQLiteDatabaseQueryInstance FindUserForAuth( m_GlobalContext->Database, _T("FindUserForAuth") );
		FindUserForAuth->Bind( "@UserUID", UserUID );

		FindUserForAuth->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				Enabled = query.GetColumnValueInt( 3 );
				Admin = query.GetColumnValueInt( 4 );
				Success = true;
				return true;
			}
		);

		if( !Success || !Enabled )
		{
			res.code = 401;
			res.end();
			return;
		}
	}

	{
		StringT SessionToken = GetRandomToken();
		StringT CSRFToken = GetRandomToken();

		auto Now = std::chrono::system_clock::now().time_since_epoch();
		auto UTCTimeNow = std::chrono::duration_cast<std::chrono::seconds>( Now ).count();

		SQLiteDatabaseQueryInstance CreateSession( m_GlobalContext->Database, _T("CreateSession") );
		CreateSession->Bind( "@SessionToken", SessionToken.c_str() );
		CreateSession->Bind( "@CSRFToken", CSRFToken.c_str() );
		CreateSession->Bind( "@UserUID", UserUID );
		CreateSession->Bind( "@LastUsed", (int64_t)UTCTimeNow );

		CreateSession->Execute( nullptr );

		std::string PortName = std::to_string( m_Port );
		std::string SessionTokenValue = "SessionToken-" + PortName;

		std::string MaxAge = std::to_string( 60 * 60 * 24 * 30 * 2 ); // +2 Months
		res.add_header( "Set-Cookie", SessionTokenValue + "=" + StringToAnsi( SessionToken ) + "; HttpOnly; Path=/; max-age=" + MaxAge + ";" );

		res.set_header( "Content-Type", "application/json" );
		res.body = "{}";
		res.code = 200;
		res.end();
	}
}

void CrowListener::HandleAuthLogout( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::string SessionTokenStr = CrowAuth::GetSessionToken( req, m_Port );
	StringT SessionToken( SessionTokenStr.begin(), SessionTokenStr.end() );

	SQLiteDatabaseQueryInstance DeleteSession( m_GlobalContext->Database, _T("DeleteSession") );
	DeleteSession->Bind( "@SessionToken", SessionToken.c_str() );
	DeleteSession->Execute( nullptr );

	std::string PortName = std::to_string( m_Port );
	res.add_header( "Set-Cookie", "SessionToken-" + PortName + "=; Max-Age=0" );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleAuthGetProfile( const crow::request& req, crow::response& res )
{
	// DO NOT CHECK CSRF HERE
	std::string SessionTokenStr = CrowAuth::GetSessionToken( req, m_Port );
	StringT SessionToken( SessionTokenStr.begin(), SessionTokenStr.end() );

	int UserUID = -1;
	StringT Username;
	StringT CSRFToken;

	bool SessionFound = false;

	{
		SQLiteDatabaseQueryInstance FindSession( m_GlobalContext->Database, _T("FindSession") );
		FindSession->Bind( "@SessionToken", SessionToken.c_str() );

		FindSession->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				CSRFToken = query.GetColumnValueText( 1 );
				UserUID = query.GetColumnValueInt( 2 );
				SessionFound = true;
				return true;
			}
		);
	}

	StringT DisplayName;
	int Enabled = 0;
	int Admin = 0;

	bool UserFound = false;

	{
		SQLiteDatabaseQueryInstance FindUserForAuth( m_GlobalContext->Database, _T("FindUserForAuth") );
		FindUserForAuth->Bind( "@UserUID", UserUID );

		FindUserForAuth->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				Username = query.GetColumnValueText( 1 );
				DisplayName = query.GetColumnValueText( 2 );
				Enabled = query.GetColumnValueInt( 3 );
				Admin = query.GetColumnValueInt( 4 );
				UserFound = true;
				return true;
			}
		);
	}

	if( SessionFound && UserFound && Enabled )
	{
		crow::json::wvalue ResponseBody;
		ResponseBody["csrf"] = StringToAnsi( CSRFToken );
		ResponseBody["username"] = StringToAnsi( Username );
		ResponseBody["userUid"] = UserUID;
		ResponseBody["admin"] = Admin;
		ResponseBody["displayName"] = StringToAnsi( DisplayName );

		res.set_header( "Content-Type", "application/json" );
		res.body = ResponseBody.dump();
		res.code = 200;
		res.end();
	}
	else
	{
		std::string PortName = std::to_string( m_Port );
		res.add_header( "Set-Cookie", "SessionToken-" + PortName + "=; Max-Age=0" );

		res.set_header( "Content-Type", "application/json" );
		res.body = "{}";
		res.code = 401;
		res.end();
	}
}

void CrowListener::HandleAuthEnumUsers( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	struct UserLookup { int UserUID; int ArrayIndex; };

	std::vector<crow::json::wvalue> Array;
	std::vector<UserLookup> LookupArray;

	SQLiteDatabaseQueryInstance FindUsers( m_GlobalContext->Database, _T("FindUsers") );

	FindUsers->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			crow::json::wvalue User;
			int uid = query.GetColumnValueInt( 0 );
			User["userid"] = uid;
			User["username"] = StringToAnsi( query.GetColumnValueText( 1 ) );
			User["displayName"] = StringToAnsi( query.GetColumnValueText( 2 ) );
			User["enabled"] = query.GetColumnValueInt( 3 );
			User["admin"] = query.GetColumnValueInt( 4 );

			Array.push_back( std::move( User ) );
			LookupArray.push_back( UserLookup{ uid, (int)Array.size() - 1 } );

			return true;
		}
	);

	for( auto& Lookup : LookupArray )
	{
		SQLiteDatabaseQueryInstance SelectGroupsForUser( m_GlobalContext->Database, _T("SelectGroupsForUser") );
		SelectGroupsForUser->Bind( "@User", Lookup.UserUID );

		std::vector<crow::json::wvalue> Groups;

		SelectGroupsForUser->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				int Group = query.GetColumnValueInt( 1 );
				Groups.push_back( Group );
				return true;
			}
		);

		Array[Lookup.ArrayIndex]["groups"] = std::move( Groups );
	}

	crow::json::wvalue Result = std::move( Array );
	res.set_header( "Content-Type", "application/json" );
	res.body = Result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleAuthNewUser( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("username") )
	{
		res.code = 400;
		res.body = "Missing username";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	StringT Username( UsernameStr.begin(), UsernameStr.end() );

	// Generate random password
	constexpr char PasswordCharacters[] = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789!$%@?+-&";
	constexpr int PasswordCharacterCount = sizeof(PasswordCharacters) - 1;
	const int DefaultPasswordLength = 8;
	unsigned char TokenBytes[DefaultPasswordLength];
	char PasswordChars[DefaultPasswordLength + 1];

	randombytes_buf( TokenBytes, sizeof(TokenBytes) );
	for( int i = 0; i < DefaultPasswordLength; i++ )
	{
		PasswordChars[i] = PasswordCharacters[TokenBytes[i] % PasswordCharacterCount];
	}
	PasswordChars[DefaultPasswordLength] = '\0';

	std::string PasswordStr = PasswordChars;
	StringT Password( PasswordStr.begin(), PasswordStr.end() );

	StringT UsernameLC = Username;
	std::transform( UsernameLC.begin(), UsernameLC.end(), UsernameLC.begin(), ::tolower );

	StringT Hash = GetHashedPasswordKey_Algorithm0( UsernameLC, Password );

	int64_t RowResult = 0;

	{
		SQLiteDatabaseQueryInstance CreateUser( m_GlobalContext->Database, _T("CreateUser") );
		CreateUser->Bind( "@Username", UsernameLC.c_str() );
		CreateUser->Bind( "@DisplayName", Username.c_str() );
		CreateUser->Bind( "@PasswordHash", Hash.c_str() );
		CreateUser->Bind( "@HashMethod", 0 );
		CreateUser->Bind( "@Enabled", 1 );
		CreateUser->Bind( "@Admin", 0 );

		if( CreateUser->Execute( nullptr ) < 0 )
		{
			crow::json::wvalue Data;
			Data["errorMessage"] = StringToAnsi( CreateUser->GetLastError() );
			res.set_header( "Content-Type", "application/json" );
			res.body = Data.dump();
			res.code = 400;
			res.end();
			return;
		}

		RowResult = CreateUser->GetLastInsertionId();
	}

	crow::json::wvalue Data;
	Data["id"] = RowResult;
	Data["username"] = StringToAnsi( UsernameLC );
	Data["displayName"] = UsernameStr;
	Data["password"] = PasswordStr;

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = RowResult > 0 ? 200 : 400;
	res.end();
}

void CrowListener::HandleAuthChangePassword( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	// Original code was empty/unimplemented
	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleAuthToggleEnabled( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("username") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	bool Value = body["value"].b();
	StringT Username( UsernameStr.begin(), UsernameStr.end() );

	SQLiteDatabaseQueryInstance SetUserEnabledState( m_GlobalContext->Database, _T("SetUserEnabledState") );
	SetUserEnabledState->Bind( "@Username", Username.c_str() );
	SetUserEnabledState->Bind( "@Enabled", Value ? 1 : 0 );

	int Result = SetUserEnabledState->Execute(
		[&]( const SQLiteDatabaseQuery& query ) { return true; }
	);

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Result >= 0 ? 200 : 500;
	res.end();
}

void CrowListener::HandleAuthToggleAdmin( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("username") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	bool Value = body["value"].b();
	StringT Username( UsernameStr.begin(), UsernameStr.end() );

	SQLiteDatabaseQueryInstance SetUserAdminState( m_GlobalContext->Database, _T("SetUserAdminState") );
	SetUserAdminState->Bind( "@Username", Username.c_str() );
	SetUserAdminState->Bind( "@Admin", Value ? 1 : 0 );

	int Result = SetUserAdminState->Execute(
		[&]( const SQLiteDatabaseQuery& query ) { return true; }
	);

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Result >= 0 ? 200 : 500;
	res.end();
}

void CrowListener::HandleAuthSetDisplayName( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("username") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string UsernameStr = body["username"].s();
	std::string DisplayNameStr = body["value"].s();
	StringT Username( UsernameStr.begin(), UsernameStr.end() );
	StringT DisplayName( DisplayNameStr.begin(), DisplayNameStr.end() );

	SQLiteDatabaseQueryInstance SetUserDisplayName( m_GlobalContext->Database, _T("SetUserDisplayName") );
	SetUserDisplayName->Bind( "@Username", Username.c_str() );
	SetUserDisplayName->Bind( "@DisplayName", DisplayName.c_str() );

	int Result = SetUserDisplayName->Execute(
		[&]( const SQLiteDatabaseQuery& query ) { return true; }
	);

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Result >= 0 ? 200 : 500;
	res.end();
}

void CrowListener::HandleAuthSetUserGroups( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int LoggedInUserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( LoggedInUserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("userid") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	int UserUID = (int)body["userid"].i();

	std::vector<int> UserGroupsRequested;
	for( auto& Element : body["value"] )
	{
		UserGroupsRequested.push_back( (int)Element.i() );
	}

	std::vector<int> UserGroupsCurrent;

	SQLiteDatabaseQueryInstance SelectGroupsForUser( m_GlobalContext->Database, _T("SelectGroupsForUser") );
	SelectGroupsForUser->Bind( "@User", UserUID );

	SelectGroupsForUser->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			int Group = query.GetColumnValueInt( 1 );
			UserGroupsCurrent.push_back( Group );
			return true;
		}
	);

	for( int Value : UserGroupsRequested )
	{
		if( find( UserGroupsCurrent.begin(), UserGroupsCurrent.end(), Value ) == UserGroupsCurrent.end() )
		{
			SQLiteDatabaseQueryInstance CreateMapping( m_GlobalContext->Database, _T("CreateUserGroupMapping") );
			CreateMapping->Bind( "@UserUID", UserUID );
			CreateMapping->Bind( "@Group", Value );
			CreateMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	for( int Value : UserGroupsCurrent )
	{
		if( find( UserGroupsRequested.begin(), UserGroupsRequested.end(), Value ) == UserGroupsRequested.end() )
		{
			SQLiteDatabaseQueryInstance DeleteMapping( m_GlobalContext->Database, _T("DeleteUserGroupMapping") );
			DeleteMapping->Bind( "@UserUID", UserUID );
			DeleteMapping->Bind( "@Group", Value );
			DeleteMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

// ===== Clip Handlers =====

static StringT GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video )
{
	StringStreamT Stream;
	Stream << CameraID;

	if( Video )
	{
		if( Manual )
			Stream << _T("_Manual");
		else
			Stream << _T("_Auto");
	}

	Stream << _T("_") << Timestamp;

	if( Video )
		Stream << _T(".mp4");
	else
		Stream << _T(".jpg");

	return (fs::path( Context.CachePath ) / Stream.str()).native();
}

void CrowListener::HandleClipThumbnail( const crow::request& req, crow::response& res, int cameraId, const std::string& clipId, bool video )
{
	uint64_t TargetCameraTimestamp = std::stoull( clipId );

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	// Try in-memory thumbnail first (non-video only)
	if( !video )
	{
		auto CameraState = m_GlobalContext->FindCameraById( cameraId );
		if( CameraState )
		{
			const auto& Camera = CameraState->ClipThumbnails;
			auto IterClip = Camera.find( TargetCameraTimestamp );
			if( IterClip != Camera.end() && (*IterClip).second.size() != 0 )
			{
				res.set_header( "Content-Type", "image/jpeg" );
				res.set_header( "Cache-Control", "no-cache, no-store, must-revalidate" );
				res.body.assign( (const char*)(*IterClip).second.data(), (*IterClip).second.size() );
				res.code = 200;
				res.end();
				return;
			}
		}
	}

	// Look up from database
	SQLiteDatabaseQueryInstance SelectClip( m_GlobalContext->Database, _T("SelectClip") );
	SelectClip->Bind( "@CameraID", cameraId );
	SelectClip->Bind( "@Timestamp", (int64_t)TargetCameraTimestamp );

	StringT ClipFilename;
	bool Success = false;

	SelectClip->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			int64_t Timestamp = query.GetColumnValueInt64( 1 );
			int CameraID = query.GetColumnValueInt( 2 );
			int RecordMode = query.GetColumnValueInt( 6 );

			ClipFilename = GetClipName( *m_GlobalContext, CameraID, Timestamp, RecordMode == 0, video );
			Success = true;
			return true;
		}
	);

	if( Success && fs::exists( ClipFilename ) )
	{
		std::ifstream file( ClipFilename, std::ios::binary );
		if( file )
		{
			std::string body( (std::istreambuf_iterator<char>(file)),
				std::istreambuf_iterator<char>() );

			res.set_header( "Content-Type", video ? "video/mp4" : "image/jpeg" );
			res.body = std::move( body );
			res.code = 200;
			res.end();
			return;
		}
	}

	res.code = 404;
	res.end();
}

void CrowListener::HandleClipEnum( const crow::request& req, crow::response& res, int cameraId, int maxCount, const std::string& startDate, const std::string& rangePeriod, int pageOffset )
{
	const int MaxClipsPerQuery = 100;
	uint64_t StartDateInt = std::stoull( startDate );
	uint64_t RangePeriodInt = std::stoull( rangePeriod );
	maxCount = std::min( maxCount, MaxClipsPerQuery );

	int UserUID = 0;
	if( cameraId == -1 )
	{
		UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
			CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
		if( UserUID < 0 )
		{
			res.code = 400;
			res.end();
			return;
		}
	}
	else
	{
		UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
			CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
		if( UserUID <= 0 )
		{
			res.code = 403;
			res.end();
			return;
		}
	}

	int Count = 0;
	std::vector<crow::json::wvalue> Array;

	{
		SQLiteDatabaseQueryInstance CountClips( m_GlobalContext->Database, cameraId == -1 ? _T("CountClipsWithinRangeAll") : _T("CountClipsWithinRange") );
		if( cameraId == -1 )
			CountClips->Bind( "@UserUID", UserUID );
		else
			CountClips->Bind( "@CameraID", cameraId );
		CountClips->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		CountClips->Bind( "@TimestampTo", (int64_t)StartDateInt );
		CountClips->Bind( "@MaxCount", maxCount );
		CountClips->Bind( "@PageOffset", pageOffset );

		CountClips->Execute(
			[&Count]( const SQLiteDatabaseQuery& query )
			{
				Count = query.GetColumnValueInt( 0 );
				return true;
			}
		);
	}

	if( Count > 0 )
	{
		SQLiteDatabaseQueryInstance SelectClips( m_GlobalContext->Database, cameraId == -1 ? _T("SelectClipsWithinRangeAll") : _T("SelectClipsWithinRange") );
		if( cameraId == -1 )
			SelectClips->Bind( "@UserUID", UserUID );
		else
			SelectClips->Bind( "@CameraID", cameraId );
		SelectClips->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		SelectClips->Bind( "@TimestampTo", (int64_t)StartDateInt );
		SelectClips->Bind( "@MaxCount", maxCount );
		SelectClips->Bind( "@PageOffset", pageOffset );

		SelectClips->Execute(
			[&Array]( const SQLiteDatabaseQuery& query )
			{
				uint64_t ClipID = query.GetColumnValueInt64( 0 );
				uint64_t Timestamp = query.GetColumnValueInt64( 1 );
				int CameraID = query.GetColumnValueInt( 2 );
				uint64_t MotionTimestamp = query.GetColumnValueInt64( 3 );
				int ActiveDuration = query.GetColumnValueInt( 4 );
				int Duration = query.GetColumnValueInt( 5 );
				int RecordMode = query.GetColumnValueInt( 6 );
				double MaxMotion = query.GetColumnValueDouble( 7 );

				const wchar_t* DescriptionStr = query.GetColumnValueText( 8 );
				std::string Description = DescriptionStr ? StringToAnsi( DescriptionStr ) : "";

				int Saved = query.GetColumnValueInt( 9 );

				const wchar_t* TagsStr = query.GetColumnValueText( 10 );
				std::string Tags = TagsStr ? StringToAnsi( TagsStr ) : "";

				crow::json::wvalue Clip;
				Clip["clipUID"] = ClipID;
				Clip["timestamp"] = Timestamp;
				Clip["cameraID"] = CameraID;
				Clip["motionTimestamp"] = MotionTimestamp;
				Clip["activeDuration"] = ActiveDuration;
				Clip["duration"] = Duration;
				Clip["recordMode"] = RecordMode;
				Clip["maxMotion"] = MaxMotion;
				Clip["description"] = Description;
				Clip["saved"] = Saved;
				Clip["tags"] = Tags;

				Array.push_back( std::move( Clip ) );
				return true;
			}
		);
	}

	crow::json::wvalue Data;
	Data["count"] = Count;
	Data["clips"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipToggleSave( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body || !body.has("id") || !body.has("value") )
	{
		res.code = 400;
		res.end();
		return;
	}

	int ClipUID = (int)body["id"].i();
	bool Value = body["value"].b();

	// Get camera ID for auth check
	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, _T("SelectClipID") );
		SelectClipID->Bind( "@ClipUID", ClipUID );
		SelectClipID->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt( 2 );
				return true;
			}
		);
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, TargetCameraInt );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	SQLiteDatabaseQueryInstance SetClipSaveState( m_GlobalContext->Database, _T("SetClipSaveState") );
	SetClipSaveState->Bind( "@ClipUID", ClipUID );
	SetClipSaveState->Bind( "@Save", Value ? 1 : 0 );
	SetClipSaveState->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body || !body.has("id") )
	{
		res.code = 400;
		res.end();
		return;
	}

	int ClipUID = (int)body["id"].i();

	// Get camera ID for auth check
	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( m_GlobalContext->Database, _T("SelectClipID") );
		SelectClipID->Bind( "@ClipUID", ClipUID );
		SelectClipID->Execute(
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt( 2 );
				return true;
			}
		);
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, TargetCameraInt );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int CameraID = 0;
	int64_t Timestamp = 0;
	bool Manual = false;

	SQLiteDatabaseQueryInstance FindClipByUID( m_GlobalContext->Database, _T("FindClipByUID") );
	FindClipByUID->Bind( "@ClipUID", ClipUID );
	FindClipByUID->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			Timestamp = query.GetColumnValueInt64( 1 );
			CameraID = query.GetColumnValueInt( 2 );
			Manual = query.GetColumnValueInt( 6 ) == 0;
			return true;
		}
	);

	// Delete files
	auto ThumbnailPath = GetClipName( *m_GlobalContext, CameraID, Timestamp, Manual, false );
	auto VideoPath = GetClipName( *m_GlobalContext, CameraID, Timestamp, Manual, true );

	std::error_code error;
	std::filesystem::remove( ThumbnailPath, error );
	std::filesystem::remove( VideoPath, error );

	SQLiteDatabaseQueryInstance DeleteClipQuery( m_GlobalContext->Database, _T("DeleteClip") );
	DeleteClipQuery->Bind( "@ClipUID", ClipUID );
	DeleteClipQuery->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

// ===== Group Handlers =====

void CrowListener::HandleGroupEnum( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::vector<crow::json::wvalue> Array;

	SQLiteDatabaseQueryInstance SelectAllGroups( m_GlobalContext->Database, _T("SelectAllGroups") );
	SelectAllGroups->Execute(
		[&Array]( const SQLiteDatabaseQuery& query )
		{
			uint64_t GroupUID = query.GetColumnValueInt64( 0 );
			std::string DisplayName = StringToAnsi( query.GetColumnValueText( 1 ) );
			std::string Description = StringToAnsi( query.GetColumnValueText( 2 ) );

			crow::json::wvalue Group;
			Group["id"] = GroupUID;
			Group["displayName"] = DisplayName;
			Group["description"] = Description;

			Array.push_back( std::move( Group ) );
			return true;
		}
	);

	crow::json::wvalue Data;
	Data["count"] = Array.size();
	Data["groups"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleGroupCreate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	StringT DisplayNameW( DisplayName.begin(), DisplayName.end() );
	StringT DescriptionW( Description.begin(), Description.end() );

	SQLiteDatabaseQueryInstance CreateGroup( m_GlobalContext->Database, _T("CreateGroup") );
	CreateGroup->Bind( "@DisplayName", DisplayNameW.c_str() );
	CreateGroup->Bind( "@Description", DescriptionW.c_str() );

	if( CreateGroup->Execute( nullptr ) < 0 )
	{
		crow::json::wvalue Data;
		Data["errorMessage"] = StringToAnsi( CreateGroup->GetLastError() );
		res.set_header( "Content-Type", "application/json" );
		res.body = Data.dump();
		res.code = 400;
		res.end();
		return;
	}

	int64_t RowResult = CreateGroup->GetLastInsertionId();

	crow::json::wvalue Data;
	Data["id"] = RowResult;

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = RowResult > 0 ? 200 : 400;
	res.end();
}

void CrowListener::HandleGroupUpdate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	int GroupUID = body.has("id") ? (int)body["id"].i() : 0;
	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	StringT DisplayNameW( DisplayName.begin(), DisplayName.end() );
	StringT DescriptionW( Description.begin(), Description.end() );

	SQLiteDatabaseQueryInstance UpdateGroup( m_GlobalContext->Database, _T("UpdateGroup") );
	UpdateGroup->Bind( "@GroupUID", GroupUID );
	UpdateGroup->Bind( "@DisplayName", DisplayNameW.c_str() );
	UpdateGroup->Bind( "@Description", DescriptionW.c_str() );
	UpdateGroup->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleGroupDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	int GroupUID = body.has("id") ? (int)body["id"].i() : 0;

	SQLiteDatabaseQueryInstance DeleteGroup( m_GlobalContext->Database, _T("DeleteGroup") );
	DeleteGroup->Bind( "@GroupUID", GroupUID );
	DeleteGroup->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

// ===== Debug Handlers =====

void CrowListener::HandleDebugEnum( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::vector<crow::json::wvalue> Array;

	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		crow::json::wvalue ValueOut;
		ValueOut["name"] = Value->GetName();
		ValueOut["value"] = Value->Get();
		Array.push_back( std::move( ValueOut ) );
	}

	crow::json::wvalue Data;
	Data["values"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugSet( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("name") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing fields";
		res.end();
		return;
	}

	std::string Name = body["name"].s();
	std::string ValueIn = body["value"].s();

	bool Success = false;
	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		if( Name == Value->GetName() )
		{
			Success = Value->Set( ValueIn.c_str() );
			break;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Success ? 200 : 400;
	res.end();
}

void CrowListener::HandleDebugReset( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("name") )
	{
		res.code = 400;
		res.body = "Missing name field";
		res.end();
		return;
	}

	std::string Name = body["name"].s();

	bool Success = false;
	const auto& Values = m_DebugConsole->GetValues();
	for( const auto& Value : Values )
	{
		if( Name == Value->GetName() )
		{
			Value->Reset();
			Success = true;
			break;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = Success ? 200 : 400;
	res.end();
}

void CrowListener::Start()
{
	// Crow/ASIO requires a numeric IP, not a hostname
	std::string bindAddr = m_Hostname;
	if( bindAddr == "localhost" )
		bindAddr = "127.0.0.1";
	else if( bindAddr == "+" || bindAddr == "*" || bindAddr == "0.0.0.0" )
		bindAddr = "0.0.0.0";

	m_App.bindaddr( bindAddr ).port( m_Port );

	m_ServerThread = std::thread( [this]()
	{
		m_App.loglevel( crow::LogLevel::Warning );
		m_App.multithreaded().run();
	});

	printf( "Crow server started on %s:%d\n", m_Hostname.c_str(), m_Port );
}

void CrowListener::Stop()
{
	m_App.stop();

	if( m_ServerThread.joinable() )
	{
		m_ServerThread.join();
	}
}
