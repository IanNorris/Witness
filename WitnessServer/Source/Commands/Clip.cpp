#include "Clip.h"
#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "sodium.h"

#include <iostream>
#include <chrono>

using namespace web::json;
using namespace web::http::client;

void Command_Clip::OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( ChildPath.size() == 3 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("thumb") ) == 0 )
		{
			OnThumbnailMessage( Context, Message, ChildPath[1], ChildPath[2], Packet );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}

void Command_Clip::OnThumbnailMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& TargetCamera, const string_t& TargetClip, const json::value& Packet )
{
	//NO CSRF!

	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	uint64_t TargetCameraTimestamp = _wtoll( TargetClip.c_str() );

	/*if( !Command_Authenticate::IsAuthenticated( Context, Message, Packet, false ) )
	{
		return;
	}*/

	lock_guard<mutex> Lock( Context->Mutex );

	auto IterCamera = Context->Cameras.find( TargetCameraInt );
	if( IterCamera != Context->Cameras.end() )
	{
		const auto& Camera = (*IterCamera).second.ClipThumbnails;
		auto IterClip = Camera.find( TargetCameraTimestamp );
		if( IterClip != Camera.end() )
		{
			http_response Response;
			Response.set_status_code( status_codes::OK );
			Response.set_body( (*IterClip).second );
			Response.headers().set_content_type( _T("image/jpeg") );
			Response.headers().set_cache_control( _T("no-cache, no-store, must-revalidate") );

			Message.reply( Response );
		}
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}
