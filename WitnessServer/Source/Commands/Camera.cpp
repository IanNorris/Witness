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

	if( ChildPath.size() == 2 && IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("record") ) == 0 )
		{
			OnRecordMessage( Context, Message, ChildPath[1], Packet );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}
	}
	else if( ChildPath.size() == 2 && !IsPost )
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
	else if( ChildPath.size() == 1 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("enum") ) == 0 )
		{
			OnEnumMessage( Context, Message, ChildPath[1], Packet );
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

	auto Iter = Context->Cameras.find( TargetCameraInt );
	if( Iter != Context->Cameras.end() )
	{
		http_response Response;
		Response.set_status_code( status_codes::OK );
		Response.set_body( (*Iter).second.PreviewThumbnail );
		Response.headers().set_content_type( _T("image/jpeg") );
		Response.headers().set_cache_control( _T("no-cache, no-store, must-revalidate") );

		Message.reply( Response );
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}

void Command_Camera::OnEnumMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet )
{
	//NO CSRF!
	if( !Command_Authenticate::IsAuthenticated( Context, Message, Packet, false ) )
	{
		return;
	}

	vector<json::value> Array;
	
	SQLiteDatabaseQueryInstance GetCameras( Context->Database, _T("GetCameras") );

	GetCameras->Execute( 
		[&Array, &Context]( const SQLiteDatabaseQuery& query )
		{
			int ID = query.GetColumnValueInt(0);
			string_t Name = query.GetColumnValueText(1);
			
			json::value Camera;
			Camera[ _T("id") ] = json::value(ID);
			Camera[ _T("name") ] = json::value(Name);

			{
				lock_guard<mutex> Lock( Context->Mutex );

				auto Iter = Context->Cameras.find( ID );
				if( Iter != Context->Cameras.end() )
				{
					Camera[ _T("recording") ] = json::value( (*Iter).second.IsRecording );
				}
			}

			Array.push_back( Camera );
				
			return true;
		} 
	);

	Message.reply( status_codes::OK, json::value::array(Array) );
}

void Command_Camera::OnRecordMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet )
{
	if( !Command_Authenticate::IsAuthenticated( Context, Message, Packet, true ) )
	{
		return;
	}

	int TargetCameraInt = _wtoi( TargetCamera.c_str() );

	bool Success = true;
	bool Record = false;
	string_t Errors;

	Success &= GetJsonField( Packet, _T("record"), Record, Errors );
	
	if (!Success)
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	bool ValidCamera = false;

	{
		lock_guard<mutex> Lock( Context->Mutex );

		auto Iter = Context->Cameras.find( TargetCameraInt );
		if( Iter != Context->Cameras.end() )
		{
			//Don't set recording value here, need to ensure it gets toggled correctly
			ValidCamera = true;
		}
	}

	auto ToggleRecord = make_shared<CameraStateToggleRecordMessage>( TargetCameraInt, Record );

	Context->MessageBus->SendToClient( nullptr, ToggleRecord );

	Message.reply( status_codes::OK, json::value(_T("OK")) );
}
