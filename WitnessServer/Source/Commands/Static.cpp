#include "Static.h"
#include "../Common.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/filestream.h"
#include "cpprest/asyncrt_utils.h"

#include <algorithm>
#include <iostream>
#include <filesystem>

using namespace web::json;
using namespace web::http::client;
using namespace utility;

namespace fs = std::filesystem;

Command_Static::Command_Static( const std::unordered_map< string_t, string_t >& Settings )
{
	string_t Errors;

	GetSettingsField(Settings, _T("server_root"), m_root, Errors);

	std::unordered_map< string_t, string_t > MimeTypes;
	MimeTypes[_T("css")] = _T("text/css");
	MimeTypes[_T("html")] = _T("text/html");
	MimeTypes[_T("js")] = _T("application/javascript");
	MimeTypes[_T("svg")] = _T("image/svg+xml");
	
	std::error_code result;
	for (auto& Iter : fs::recursive_directory_iterator(m_root, result))
	{
		if( !fs::is_directory(Iter) )
		{
			auto Path = Iter.path();
			string_t PathString = string_t( Path.native() );

			if( PathString.compare(0, m_root.length(), m_root, 0 ) == 0 )
			{
				PathString = PathString.substr( m_root.length() );
			}

			for_each( PathString.begin(), PathString.end(), []( char_t& Char ) { Char = (Char == '\\') ? '/' : Char; } );

			if (PathString.length() > 1 && PathString[0] == '/' )
			{
				PathString = PathString.substr(1);
			}

			string_t ContentType = U("application/octet-stream");

			if( Path.has_extension() )
			{
				string_t PathExt = Path.extension().native().substr(1);

				if (MimeTypes.find(PathExt) != MimeTypes.end())
				{
					ContentType = MimeTypes[PathExt];
				}
			}

			m_staticDataPaths[PathString] = ContentType;
		}
	}

	if( result )
	{
		auto message = result.message();
		std::tcerr << string_t( message.begin(), message.end() ) << std::endl;
	}
}

void Command_Static::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, std::vector<string_t>& ChildPath, bool IsPost )
{
	string_t Joined;
	std::for_each( ChildPath.begin(), ChildPath.end(),
		[&Joined]( const string_t& Next )
		{
			if( !Joined.empty() )
			{
				Joined.append( U("/") );
			}

			Joined.append( Next );
		}
	);

	if( Joined.empty() )
	{
		Joined = U("index.html");
	}

	if (IsPost)
	{
		Message.reply( status_codes::NotFound );
	}
	else
	{
		for( int Pass = 0; Pass < 2; Pass++ )
		{
			auto Iter = m_staticDataPaths.find(Joined);
			if( Iter != m_staticDataPaths.end() )
			{
				fs::path fullPath = m_root;
				fullPath.append( Iter->first.c_str() );

				size64_t FileSize = fs::file_size( fullPath );

				auto FileHandle = concurrency::streams::file_stream<uint8_t>::open_istream(fullPath.native());

				Concurrency::streams::istream FileHandleStream = FileHandle.get(); 

				http_response response(status_codes::OK);
				//response.headers().add(U("Content-Security-Policy"), U("default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval'; worker-src 'self' blob:; style-src 'self' 'unsafe-inline'; script-src-elem 'self' 'unsafe-inline' 'unsafe-eval'; style-src-attr 'self' 'unsafe-inline'; img-src 'self' data: 'self' blob:; font-src 'self' data:; media-src 'self' blob:;"));

				response.headers().add(U("Content-Security-Policy"), U("default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; worker-src 'self' blob:; style-src 'self' 'unsafe-inline' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; script-src-elem 'self' 'unsafe-inline' 'unsafe-eval' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; style-src-attr 'self' 'unsafe-inline' https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; img-src 'self' data: 'self' blob: https://maxcdn.bootstrapcdn.com https://ajax.googleapis.com/ https://cdnjs.cloudflare.com/ https://cloud.githubusercontent.com/; font-src 'self' data:; media-src 'self' blob:;"));

				//https://maxcdn.bootstrapcdn.com/
				
				response.set_body(FileHandleStream, FileSize, Iter->second);
				Message.reply(response);

				return;
			}

			if( Pass == 0 )
			{
				Joined.append(U("/index.html"));
			}
		}

		Message.reply( status_codes::NotFound );
	}
}
