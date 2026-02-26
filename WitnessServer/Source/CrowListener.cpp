#include "CrowListener.h"
#include "CameraWorker.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <format>
#include <cmath>

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

void CrowListener::Start()
{
	m_App.bindaddr( m_Hostname ).port( m_Port );

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
