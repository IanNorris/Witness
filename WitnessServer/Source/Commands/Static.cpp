#include "Static.h"
#include "../Common.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/filestream.h"
#include "cpprest/asyncrt_utils.h"

#include <algorithm>
#include <iostream>
#include <experimental/filesystem>

using namespace web::json;
using namespace web::http::client;
using namespace utility;
using namespace std;

namespace fs = std::experimental::filesystem;

Command_Static::Command_Static(object& Config)
{
	m_root = Config[U("static_path")].as_string();

	json::object& MimeTypes = Config[U("mime")].as_object();
	
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
					ContentType = MimeTypes[PathExt].as_string();
				}
			}

			m_staticDataPaths[PathString] = ContentType;
		}
	}

	if( result )
	{
		auto message = result.message();
		tcerr << string_t( message.begin(), message.end() ) << endl;
	}
}

void Command_Static::OnMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	string_t Joined;
	for_each( ChildPath.begin(), ChildPath.end(),
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
				fullPath.append( Iter->first );

				size64_t FileSize = fs::file_size( fullPath );

				auto FileHandle = concurrency::streams::file_stream<uint8_t>::open_istream(fullPath.native());

				Concurrency::streams::istream& FileHandleStream = FileHandle.get(); 

				//Matching file
				Message.reply( status_codes::OK, FileHandleStream, FileSize, Iter->second );
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
