#include "Debug.h"
#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "cpprest/filestream.h"

using namespace web::json;
using namespace web::http::client;

namespace fs = std::experimental::filesystem;

Command_Debug::Command_Debug( DebugConsole* DebugConsoleInstance )
: DebugConsoleInstance( DebugConsoleInstance )
{
}

void Command_Debug::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	if( ChildPath.size() == 1 )
	{
		auto Command = ChildPath.front();

		auto Packet = Message.extract_json().get();

		if( IsPost )
		{
			if( Command.compare( _T("set") ) == 0 )
			{
				OnSetMessage( Context, Message, Packet );
			}
			else if( Command.compare( _T("reset") ) == 0 )
			{
				OnResetMessage( Context, Message, Packet );
			}
			else
			{
				Message.reply( status_codes::NotFound );
			}

			return;
		}
		else
		{
			if( Command.compare( _T("enum") ) == 0 )
			{
				OnEnumMessage( Context, Message, Packet );
			}
			else 
			{
				Message.reply( status_codes::NotFound );
			}

			return;
		}
	}

	Message.reply( status_codes::NotFound );
}

void Command_Debug::OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}

	json::value Data;
	vector<json::value> Array;

	const auto& Values = DebugConsoleInstance->GetValues();
	for( const auto& Value : Values )
	{
		json::value ValueOut;

		const string Name = Value->GetName();
		const string ValueStr = Value->Get();

		ValueOut[ _T("name") ] = json::value( string_t( Name.begin(), Name.end() ) );
		ValueOut[ _T("value") ] = json::value( string_t( ValueStr.begin(), ValueStr.end() ) );

		Array.push_back( ValueOut );
	}

	Data[ _T("values") ] = json::value::array(Array);

	Message.reply( status_codes::OK, Data );
}

void Command_Debug::OnSetMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}
	json::value Data;

	bool Success = true;

	string_t Errors;
	
	string_t Name;
	string_t ValueIn;
		
	Success &= GetJsonField( Packet, _T("name"), Name, Errors );
	Success &= GetJsonField( Packet, _T("value"), ValueIn, Errors );

	if( !Success )
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	Success = false;
	const auto& Values = DebugConsoleInstance->GetValues();
	for( const auto& Value : Values )
	{
		json::value ValueOut;

		const string NameStr = Value->GetName();
		const string_t NameWide = string_t( NameStr.begin(), NameStr.end() );

		if( NameWide.compare( Name ) == 0 )
		{
			Success = Value->Set( string(ValueIn.begin(), ValueIn.end()).c_str() );

			break;
		}
	}
	
	if( Success )
	{
		Message.reply( status_codes::OK, Data );
	}
	else
	{
		Message.reply( status_codes::BadRequest );
	}
}

void Command_Debug::OnResetMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}
	json::value Data;

	bool Success = true;

	string_t Errors;
	
	string_t Name;
		
	Success &= GetJsonField( Packet, _T("name"), Name, Errors );

	if( !Success )
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	Success = false;
	const auto& Values = DebugConsoleInstance->GetValues();
	for( const auto& Value : Values )
	{
		json::value ValueOut;

		const string NameStr = Value->GetName();
		const string_t NameWide = string_t( Name.begin(), Name.end() );

		if( NameWide.compare( Name ) == 0 )
		{
			Success = true;

			Value->Reset();

			break;
		}
	}
	
	if( Success )
	{
		Message.reply( status_codes::OK, Data );
	}
	else
	{
		Message.reply( status_codes::BadRequest );
	}
}
