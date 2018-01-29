#include "Camera.h"
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

void Command_Camera::OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( ChildPath.size() == 2 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("preview") ) == 0 )
		{
			OnPreviewMessage( Context, Message, ChildPath[1], Packet );
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

void Command_Camera::OnPreviewMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet )
{
	//NO CSRF!

	int TargetCameraInt = _wtoi( TargetCamera.c_str() );

	if( !Command_Authenticate::IsAuthenticated( Context, Message, Packet, false ) )
	{
		return;
	}

	lock_guard<mutex> Lock( Context->Mutex );

	auto Iter = Context->CameraPreviews.find( TargetCameraInt );
	if( Iter != Context->CameraPreviews.end() )
	{
		http_response Response;
		Response.set_status_code( status_codes::OK );
		Response.set_body( (*Iter).second );
		Response.headers().set_content_type( _T("image/jpeg") );
		Response.headers().set_cache_control( _T("no-cache, no-store, must-revalidate") );

		Message.reply( Response );
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}
