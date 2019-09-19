#include "Group.h"
#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "cpprest/filestream.h"

using namespace web::json;
using namespace web::http::client;

namespace fs = std::experimental::filesystem;

void Command_Group::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	if( ChildPath.size() == 1 )
	{
		auto Command = ChildPath.front();

		auto Packet = Message.extract_json().get();

		if( IsPost )
		{
			if( Command.compare( _T("create") ) == 0 )
			{
				OnCreateMessage( Context, Message, Packet );
			}
			else if( Command.compare( _T("update") ) == 0 )
			{
				OnUpdateMessage( Context, Message, Packet );
			}
			else if( Command.compare( _T("delete") ) == 0 )
			{
				OnDeleteMessage( Context, Message, Packet );
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

void Command_Group::OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}

	json::value Data;
	vector<json::value> Array;

	{
		SQLiteDatabaseQueryInstance SelectAllGroups( Context.Database, _T("SelectAllGroups") );

		SelectAllGroups->Execute( 
			[&Array]( const SQLiteDatabaseQuery& query )
			{
				uint64_t GroupUID = query.GetColumnValueInt64(0);
				string_t DisplayName = query.GetColumnValueText(1);
				string_t Description = query.GetColumnValueText(2);

				json::value Camera;
				Camera[ _T("id") ] = json::value(GroupUID);
				Camera[ _T("displayName") ] = json::value(DisplayName);
				Camera[ _T("description") ] = json::value(Description);

				Array.push_back( Camera );

				return true;
			}
		);
	}
	
	Data[ _T("count") ] = json::value(Array.size());
	Data[ _T("groups") ] = json::value::array(Array);

	Message.reply( status_codes::OK, Data );
}

void Command_Group::OnCreateMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}

	string_t Errors;
	
	bool Success = true;
	string_t DisplayName;
	string_t Description;

	Success &= GetJsonField( Packet, _T("displayName"), DisplayName, Errors );
	Success &= GetJsonField( Packet, _T("description"), Description, Errors );

	json::value Data;
	
	int64_t RowResult = 0;

	{
		SQLiteDatabaseQueryInstance CreateGroup( Context.Database, _T("CreateGroup") );
		CreateGroup->Bind( "@DisplayName", DisplayName.c_str() );
		CreateGroup->Bind( "@Description", Description.c_str() );

		if (CreateGroup->Execute(nullptr) < 0)
		{
			json::value Data;
			Data[ _T("errorMessage") ] = json::value(CreateGroup->GetLastError());

			Message.reply( status_codes::BadRequest, Data );
			return;
		}
		RowResult = CreateGroup->GetLastInsertionId();
	}
	
	Data[ _T("id") ] = json::value(RowResult);

	if( RowResult > 0 )
	{
		Message.reply( status_codes::OK, Data );
	}
	else
	{
		Message.reply( status_codes::BadRequest );
	}
}

void Command_Group::OnUpdateMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}

	string_t Errors;
	
	bool Success = true;
	int GroupUID;
	string_t DisplayName;
	string_t Description;

	
	Success &= GetJsonField( Packet, _T("id"), GroupUID, Errors );
	Success &= GetJsonField( Packet, _T("displayName"), DisplayName, Errors );
	Success &= GetJsonField( Packet, _T("description"), Description, Errors );

	

	{
		SQLiteDatabaseQueryInstance UpdateGroup( Context.Database, _T("UpdateGroup") );
		UpdateGroup->Bind( "@GroupUID", GroupUID );
		UpdateGroup->Bind( "@DisplayName", DisplayName.c_str() );
		UpdateGroup->Bind( "@Description", Description.c_str() );

		UpdateGroup->Execute( nullptr );
	}

	json::value Data;
	Message.reply( status_codes::OK, Data );
}

void Command_Group::OnDeleteMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	if( Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator ) < 0 )
	{
		return;
	}

	string_t Errors;
	
	bool Success = true;
	int GroupUID;
		
	Success &= GetJsonField( Packet, _T("id"), GroupUID, Errors );
	
	int RowResult = 0;

	{
		SQLiteDatabaseQueryInstance DeleteGroup( Context.Database, _T("DeleteGroup") );
		DeleteGroup->Bind( "@GroupUID", GroupUID );

		RowResult = DeleteGroup->Execute( nullptr );
	}

	json::value Data;
	Message.reply( status_codes::OK, Data );
}
