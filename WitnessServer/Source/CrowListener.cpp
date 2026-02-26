#include "CrowListener.h"

#include <filesystem>
#include <fstream>
#include <iostream>

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
