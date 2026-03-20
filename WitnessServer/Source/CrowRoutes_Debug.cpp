#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "SQLite.h"
#include "ClipReprocessWorker.h"

#include <Log.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>

#ifdef CROW_ENABLE_SSL
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#endif
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

void CrowListener::HandleDebugReloadTLS( const crow::request& req, crow::response& res )
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

	bool Success = ReloadTLS();

	crow::json::wvalue Data;
	Data["success"] = Success;
	Data["message"] = Success ? "TLS certificate reloaded" : "TLS reload failed — check server logs";

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = Success ? 200 : 500;
	res.end();
}

void CrowListener::HandleDebugStreamingDiag( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::vector<crow::json::wvalue> CameraArray;

	{
		std::shared_lock<std::shared_mutex> lock( m_GlobalContext->Mutex );
		for( auto& [id, state] : m_GlobalContext->GetCameraMap() )
		{
			crow::json::wvalue CamData;
			CamData["id"] = id;
			CamData["name"] = state.Name;
			CamData["status"] = state.Status;

			if( state.Worker )
			{
				CamData["lowLatencyHLS"] = state.Worker->GetCameraSettings().LowLatencyHLS;

				auto& LiveStream = state.Worker->GetLiveStream();
				if( LiveStream )
				{
					auto Diag = LiveStream->GetStreamingDiagnostics();

					crow::json::wvalue StreamData;
					StreamData["totalSegments"] = Diag.TotalSegments;
					StreamData["reconnectCount"] = Diag.ReconnectCount;
					StreamData["totalDtsDuration"] = Diag.TotalDtsDuration;
					StreamData["totalAccumulatedDuration"] = Diag.TotalAccumulatedDuration;
					StreamData["cumulativeDriftMs"] = (Diag.TotalAccumulatedDuration - Diag.TotalDtsDuration) * 1000.0;
					StreamData["maxSingleSegmentDriftMs"] = Diag.MaxDriftMs;
					StreamData["currentSegmentIndex"] = Diag.CurrentSegmentIndex;
					StreamData["backlogSize"] = Diag.BacklogSize;
					StreamData["initGeneration"] = Diag.InitGeneration;

					if( Diag.TotalSegments > 0 )
					{
						StreamData["avgDtsDuration"] = Diag.TotalDtsDuration / Diag.TotalSegments;
						StreamData["avgAccumulatedDuration"] = Diag.TotalAccumulatedDuration / Diag.TotalSegments;
					}

					std::vector<crow::json::wvalue> Segments;
					for( auto& Seg : Diag.RecentSegments )
					{
						crow::json::wvalue S;
						S["idx"] = Seg.SegmentIndex;
						S["dtsDur"] = Seg.DtsDuration;
						S["accDur"] = Seg.AccumulatedDuration;
						S["driftMs"] = Seg.DriftMs;
						Segments.push_back( std::move( S ) );
					}
					StreamData["recentSegments"] = std::move( Segments );

					CamData["streaming"] = std::move( StreamData );
				}
			}

			CameraArray.push_back( std::move( CamData ) );
		}
	}

	crow::json::wvalue Data;
	Data["timestamp"] = std::format( "{:%Y-%m-%dT%H:%M:%S}", std::chrono::system_clock::now() );
	Data["cameras"] = std::move( CameraArray );

	// Read today's log file and filter for [HLS] and [DVR] lines
	std::string logDir = ::Witness::LogGetDirectory();
	if( !logDir.empty() )
	{
		auto now = std::chrono::system_clock::now();
		auto time_t = std::chrono::system_clock::to_time_t( now );
		struct tm tm_buf;
#ifdef _WIN32
		localtime_s( &tm_buf, &time_t );
#else
		localtime_r( &time_t, &tm_buf );
#endif
		char dateBuf[16];
		snprintf( dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
			tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday );

		std::string logPath = logDir + "/witness-" + dateBuf + ".log";
		std::ifstream logFile( logPath );
		if( logFile.is_open() )
		{
			std::vector<crow::json::wvalue> LogLines;
			std::string line;
			while( std::getline( logFile, line ) )
			{
				if( line.find( "[HLS]" ) != std::string::npos ||
					line.find( "[DVR]" ) != std::string::npos )
				{
					LogLines.push_back( line );
				}
			}
			Data["serverLog"] = std::move( LogLines );
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugDisk( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	crow::json::wvalue Data;

	try
	{
	// Disk space info
	std::error_code ec;
	auto spaceInfo = std::filesystem::space( m_GlobalContext->CachePath, ec );
	if( !ec )
	{
		Data["diskTotal"] = static_cast<int64_t>(spaceInfo.capacity);
		Data["diskFree"] = static_cast<int64_t>(spaceInfo.available);
		Data["diskUsed"] = static_cast<int64_t>(spaceInfo.capacity - spaceInfo.available);
		Data["diskTotalGB"] = static_cast<double>(spaceInfo.capacity) / (1024.0 * 1024 * 1024);
		Data["diskFreeGB"] = static_cast<double>(spaceInfo.available) / (1024.0 * 1024 * 1024);
	}

	// Continuous segment totals
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousTotalSize" );
		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			Data["segmentCount"] = q.GetColumnValueInt64(0);
			Data["segmentTotalDuration"] = q.GetColumnValueInt64(1);
			Data["segmentTotalBytes"] = q.GetColumnValueInt64(2);
			Data["segmentTotalGB"] = static_cast<double>(q.GetColumnValueInt64(2)) / (1024.0 * 1024 * 1024);
			return true;
		});
	}

	// Per-camera breakdown
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSizePerCamera" );
		std::vector<crow::json::wvalue> cameras;
		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			crow::json::wvalue cam;
			cam["cameraId"] = q.GetColumnValueInt64(0);
			cam["segmentCount"] = q.GetColumnValueInt64(1);
			cam["totalBytes"] = q.GetColumnValueInt64(2);
			cam["totalGB"] = static_cast<double>(q.GetColumnValueInt64(2)) / (1024.0 * 1024 * 1024);
			cameras.push_back( std::move(cam) );
			return true;
		});
		Data["cameras"] = std::move(cameras);
	}

	Data["cachePath"] = m_GlobalContext->CachePath;

	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "HandleDebugDisk exception: %s", e.what() );
		Data["error"] = std::string( e.what() );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDebugDiskScan( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	crow::json::wvalue Data;

	try
	{
		int64_t totalBytes = 0;
		int fileCount = 0;
		std::error_code ec;

		for( auto& entry : std::filesystem::recursive_directory_iterator( m_GlobalContext->CachePath, ec ) )
		{
			if( entry.is_regular_file( ec ) )
			{
				totalBytes += static_cast<int64_t>( entry.file_size( ec ) );
				fileCount++;
			}
		}

		Data["totalBytes"] = totalBytes;
		Data["totalGB"] = static_cast<double>(totalBytes) / (1024.0 * 1024 * 1024);
		Data["fileCount"] = fileCount;
	}
	catch( const std::exception& e )
	{
		Data["error"] = std::string( e.what() );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDetectionQuery( const crow::request& req, crow::response& res, int cameraId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Parse from/to query params (epoch seconds)
	double from = 0, to = 0;
	auto fromParam = req.url_params.get( "from" );
	auto toParam = req.url_params.get( "to" );

	if( !fromParam || !toParam )
	{
		res.code = 400;
		res.body = R"({"error":"Missing from/to query params"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	from = std::stod( fromParam );
	to = std::stod( toParam );

	// Query detection frames with boxes
	crow::json::wvalue result;
	std::vector<crow::json::wvalue> frames;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectDetectionFramesWithBoxes" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@TimestampFrom", from );
		query->Bind( "@TimestampTo", to );

		int64_t currentFrameUID = -1;
		crow::json::wvalue currentFrame;
		std::vector<crow::json::wvalue> currentBoxes;

		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			int64_t frameUID = q.GetColumnValueInt64( 0 );

			if( frameUID != currentFrameUID )
			{
				// Save previous frame
				if( currentFrameUID >= 0 )
				{
					currentFrame["boxes"] = std::move( currentBoxes );
					frames.push_back( std::move( currentFrame ) );
					currentBoxes.clear();

					// Limit response size
					if( frames.size() >= 2000 )
						return true;
				}

				currentFrameUID = frameUID;
				currentFrame = crow::json::wvalue();
				currentFrame["t"] = q.GetColumnValueDouble( 1 );  // timestamp
				currentFrame["w"] = q.GetColumnValueInt( 2 );     // frameWidth
				currentFrame["h"] = q.GetColumnValueInt( 3 );     // frameHeight
			}

			// Add box (LEFT JOIN may produce NULL box columns if frame has no boxes)
			const char* className = q.GetColumnValueText( 6 );
			if( className )
			{
				crow::json::wvalue box;
				box["id"] = q.GetColumnValueInt( 4 );    // TrackingID
				box["cls"] = std::string( className );
				box["conf"] = q.GetColumnValueDouble( 7 );
				box["x"] = q.GetColumnValueDouble( 8 );
				box["y"] = q.GetColumnValueDouble( 9 );
				box["w"] = q.GetColumnValueDouble( 10 );
				box["h"] = q.GetColumnValueDouble( 11 );
				box["baseline"] = q.GetColumnValueInt( 12 ) != 0;
				const char* cropPath = q.GetColumnValueText( 13 );
				if( cropPath )
					box["crop"] = std::string( cropPath );
				const char* faceName = q.GetColumnValueText( 14 );
				if( faceName )
					box["name"] = std::string( faceName );
				currentBoxes.push_back( std::move( box ) );
			}

			return true;
		});

		// Don't forget the last frame
		if( currentFrameUID >= 0 && frames.size() < 2000 )
		{
			currentFrame["boxes"] = std::move( currentBoxes );
			frames.push_back( std::move( currentFrame ) );
		}
	}
	catch( const std::exception& e )
	{
		res.code = 500;
		crow::json::wvalue err;
		err["error"] = std::string( e.what() );
		res.body = err.dump();
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	result["frames"] = std::move( frames );
	result["cameraId"] = cameraId;

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleTrailsQuery( const crow::request& req, crow::response& res, int cameraId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	if( !m_GlobalContext || !m_GlobalContext->Database )
	{
		res.code = 500;
		res.body = R"({"error":"Database not available"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	auto fromParam = req.url_params.get( "from" );
	auto toParam = req.url_params.get( "to" );

	if( !fromParam || !toParam )
	{
		res.code = 400;
		res.body = R"({"error":"Missing from/to query params"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	double from = std::stod( fromParam );
	double to = std::stod( toParam );

	// Build JSON response manually for performance — avoids crow::json::wvalue overhead
	// and passes PointData through as raw compact arrays
	std::ostringstream json;
	json << R"({"cameraId":)" << cameraId << R"(,"trails":[)";
	bool first = true;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectTrails" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@TimestampFrom", from );
		query->Bind( "@TimestampTo", to );

		query->Execute( [&]( const SQLiteDatabaseQuery& q ) -> bool
		{
			const char* pointData = q.GetColumnValueText( 7 );
			if( !pointData )
				return true;

			if( !first ) json << ",";
			first = false;

			int64_t trailUID = q.GetColumnValueInt64( 0 );
			int64_t clipUID = q.GetColumnValueInt64( 1 );
			const char* className = q.GetColumnValueText( 3 );
			const char* faceName = q.GetColumnValueText( 4 );
			double clipTimestamp = q.GetColumnValueDouble( 8 );
			double clipDuration = q.GetColumnValueDouble( 9 );

			json << R"({"id":)" << trailUID
				 << R"(,"clipId":)" << clipUID
				 << R"(,"clipTs":)" << std::fixed << std::setprecision( 2 ) << clipTimestamp
				 << R"(,"clipDur":)" << clipDuration
				 << R"(,"cls":")" << ( className ? className : "" ) << "\"";

			if( faceName )
				json << R"(,"name":")" << faceName << "\"";

			// Pass PointData through verbatim — already compact [[x,y,t],...]
			json << R"(,"pts":)" << pointData << "}";

			return true;
		});
	}
	catch( const std::exception& e )
	{
		res.code = 500;
		res.body = std::string( R"({"error":")" ) + e.what() + "\"}";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	json << "]}";

	res.set_header( "Content-Type", "application/json" );
	res.body = json.str();
	res.code = 200;
	res.end();
}

void CrowListener::HandleReprocessQueue( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	crow::json::wvalue result;

	// Get total count
	int totalCount = 0;
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "CountClipsToReprocess" );
		q->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		q->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			totalCount = query.GetColumnValueInt( 0 );
			return true;
		});
	}

	// Get top 100 queued clips
	std::vector<crow::json::wvalue> clips;
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectReprocessQueue" );
		q->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		q->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			crow::json::wvalue clip;
			clip["uid"] = query.GetColumnValueInt64( 0 );
			clip["timestamp"] = query.GetColumnValueInt64( 1 );
			clip["camera"] = query.GetColumnValueInt( 2 );
			clip["detectionVersion"] = query.GetColumnValueInt( 3 );
			clip["recordMode"] = query.GetColumnValueInt( 4 );
			const char* tags = query.GetColumnValueText( 5 );
			clip["tags"] = tags ? std::string( tags ) : "";
			clip["duration"] = query.GetColumnValueDouble( 6 );
			clips.push_back( std::move( clip ) );
			return true;
		});
	}

	result["total"] = totalCount;
	result["clips"] = std::move( clips );
	result["detectionVersion"] = CURRENT_DETECTION_VERSION;

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

#ifdef CROW_ENABLE_SSL
static bool LogCertExpiry( const std::string& certPath )
{
	FILE* fp = fopen( certPath.c_str(), "r" );
	if( !fp )
	{
		LOG_ERROR( "TLS: Unable to open certificate file: %s", certPath.c_str() );
		return false;
	}

	X509* cert = PEM_read_X509( fp, nullptr, nullptr, nullptr );
	fclose( fp );

	if( !cert )
	{
		LOG_ERROR( "TLS: Unable to parse certificate: %s", certPath.c_str() );
		return false;
	}

	const ASN1_TIME* notAfter = X509_get0_notAfter( cert );
	int pday = 0, psec = 0;
	ASN1_TIME_diff( &pday, &psec, nullptr, notAfter );

	bool expired = ( pday < 0 || ( pday == 0 && psec < 0 ) );

	if( expired )
	{
		LOG_ERROR( "TLS WARNING: Certificate has expired! (%s)", certPath.c_str() );
		LOG_ERROR( "TLS WARNING: Server will start but clients may reject the connection." );
	}
	else
	{
		LOG_INFO( "TLS: Certificate expires in %d days (%s)", pday, certPath.c_str() );
		if( pday < 30 )
		{
			LOG_WARNING( "TLS WARNING: Certificate expires in less than 30 days — consider renewing." );
		}
	}

	X509_free( cert );
	return true;
}
#endif

bool CrowListener::ConfigureSSL()
{
#ifdef CROW_ENABLE_SSL
	if( !m_Secure )
		return true;

	namespace fs = std::filesystem;

	if( m_CertPath.empty() || m_KeyPath.empty() )
	{
		LOG_ERROR( "TLS ERROR: TLS is enabled but certificate paths are not configured." );
		LOG_ERROR( "  Set server_tls_cert and server_tls_key in the database settings," );
		LOG_ERROR( "  or run Setup-TLS.ps1 to configure TLS certificates." );
		LOG_ERROR( "  To disable TLS, set server_tls_mode to NoSecurity." );
		return false;
	}

	if( !fs::exists( m_CertPath ) )
	{
		LOG_ERROR( "TLS ERROR: Certificate file not found: %s", m_CertPath.c_str() );
		return false;
	}

	if( !fs::exists( m_KeyPath ) )
	{
		LOG_ERROR( "TLS ERROR: Private key file not found: %s", m_KeyPath.c_str() );
		return false;
	}

	LogCertExpiry( m_CertPath );

	m_App.ssl_file( m_CertPath, m_KeyPath );

	// Track cert modification time for auto-reload
	std::error_code ec;
	m_LastCertModTime = fs::last_write_time( m_CertPath, ec );

	LOG_INFO( "TLS: Configured with cert=%s key=%s", m_CertPath.c_str(), m_KeyPath.c_str() );
	return true;
#else
	if( m_Secure )
	{
		LOG_ERROR( "TLS ERROR: TLS requested but CROW_ENABLE_SSL is not compiled in." );
		return false;
	}
	return true;
#endif
}

// Resolve a hostname to a bind address. ASIO requires a numeric IP.
static std::string ResolveBindAddress( const std::string& hostname )
{
	if( hostname == "localhost" )
		return "127.0.0.1";
	if( hostname == "+" || hostname == "*" || hostname == "0.0.0.0" || hostname.empty() )
		return "0.0.0.0";

	// Check if it's already a numeric IP
	std::error_code ec;
	asio::ip::make_address( hostname, ec );
	if( !ec )
		return hostname;

	// It's a domain name — bind to all interfaces
	return "0.0.0.0";
}

bool CrowListener::ReloadTLS()
{
#ifdef CROW_ENABLE_SSL
	if( !m_Secure )
	{
		LOG_INFO( "TLS reload skipped — TLS is not enabled." );
		return true;
	}

	LOG_INFO( "TLS: Reloading certificate..." );

	LogCertExpiry( m_CertPath );

	// Reload the SSL context on the underlying ASIO ssl_context
	// New connections will use the updated certificate
	try
	{
		auto* sslCtx = SSL_CTX_new( TLS_server_method() );
		if( !sslCtx )
		{
			LOG_ERROR( "TLS reload failed: unable to create new SSL context." );
			return false;
		}

		if( SSL_CTX_use_certificate_chain_file( sslCtx, m_CertPath.c_str() ) != 1 )
		{
			LOG_ERROR( "TLS reload failed: unable to load certificate." );
			SSL_CTX_free( sslCtx );
			return false;
		}

		if( SSL_CTX_use_PrivateKey_file( sslCtx, m_KeyPath.c_str(), SSL_FILETYPE_PEM ) != 1 )
		{
			LOG_ERROR( "TLS reload failed: unable to load private key." );
			SSL_CTX_free( sslCtx );
			return false;
		}

		SSL_CTX_free( sslCtx );

		// Validated successfully — now do a graceful server restart
		LOG_INFO( "TLS: Certificate validated, restarting server..." );
		m_App.stop();
		if( m_ServerThread.joinable() )
			m_ServerThread.join();

		m_App.ssl_file( m_CertPath, m_KeyPath );

		std::string bindAddr = ResolveBindAddress( m_Hostname );

		m_App.bindaddr( bindAddr ).port( m_Port );
		m_ServerThread = std::thread( [this]()
		{
			try
			{
				m_App.loglevel( crow::LogLevel::Warning );
				m_App.concurrency( m_CrowThreadCount ).run();
			}
			catch( const std::exception& e )
			{
				LOG_ERROR( "Server error after TLS reload: %s", e.what() );
			}
		});

		// Update tracked modification time
		std::error_code ec;
		m_LastCertModTime = std::filesystem::last_write_time( m_CertPath, ec );

		LOG_INFO( "TLS: Certificate reloaded successfully." );
		return true;
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "TLS reload failed: %s", e.what() );
		return false;
	}
#else
	LOG_INFO( "TLS reload skipped — CROW_ENABLE_SSL not compiled in." );
	return false;
#endif
}

void CrowListener::CertMonitorLoop()
{
#ifdef CROW_ENABLE_SSL
	namespace fs = std::filesystem;

	while( m_CertMonitorRunning.load() )
	{
		// Check every 12 hours
		for( int i = 0; i < 12 * 60 && m_CertMonitorRunning.load(); i++ )
		{
			std::this_thread::sleep_for( std::chrono::minutes(1) );
		}

		if( !m_CertMonitorRunning.load() )
			break;

		std::error_code ec;
		auto currentModTime = fs::last_write_time( m_CertPath, ec );
		if( ec )
			continue;

		if( currentModTime != m_LastCertModTime )
		{
			LOG_INFO( "TLS: Certificate file changed on disk, triggering reload..." );
			ReloadTLS();
		}
	}
#endif
}

void CrowListener::Start()
{
	if( !ConfigureSSL() )
	{
		LOG_ERROR( "Server failed to start due to TLS configuration error." );
		return;
	}

	std::string bindAddr = ResolveBindAddress( m_Hostname );

	m_CrowThreadCount = std::max( 4u, std::thread::hardware_concurrency() * 2 );
	LOG_INFO( "Crow HTTP server: %u worker threads", m_CrowThreadCount );

	try
	{
		m_App.bindaddr( bindAddr ).port( m_Port );

		m_ServerThread = std::thread( [this]()
		{
			try
			{
				m_App.loglevel( crow::LogLevel::Warning );
				m_App.concurrency( m_CrowThreadCount ).run();
			}
			catch( const std::exception& e )
			{
				LOG_ERROR( "Server error: %s", e.what() );
			}
		});
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Failed to start server on %s:%d — %s", bindAddr.c_str(), m_Port, e.what() );
		return;
	}

	LOG_INFO( "Crow server started on %s:%d (%s)", m_Hostname.c_str(), m_Port, m_Secure ? "HTTPS" : "HTTP" );

	// Start cert file monitor if TLS is active
	if( m_Secure && !m_CertPath.empty() )
	{
		m_CertMonitorRunning = true;
		m_CertMonitorThread = std::thread( &CrowListener::CertMonitorLoop, this );
	}
}

void CrowListener::Stop()
{
	// Stop cert monitor
	m_CertMonitorRunning = false;
	if( m_CertMonitorThread.joinable() )
		m_CertMonitorThread.join();

	m_App.stop();

	if( m_ServerThread.joinable() )
	{
		m_ServerThread.join();
	}
}