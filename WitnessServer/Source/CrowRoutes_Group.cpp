#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
// ===== Group Handlers =====

void CrowListener::HandleGroupEnum( const crow::request& req, crow::response& res )
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

	SQLiteDatabaseQueryInstance SelectAllGroups( m_GlobalContext->Database, "SelectAllGroups" );
	SelectAllGroups->Execute(
		[&Array]( const SQLiteDatabaseQuery& query )
		{
			uint64_t GroupUID = query.GetColumnValueInt64( 0 );
			std::string DisplayName = query.GetColumnValueText( 1 );
			std::string Description = query.GetColumnValueText( 2 );

			crow::json::wvalue Group;
			Group["id"] = GroupUID;
			Group["displayName"] = DisplayName;
			Group["description"] = Description;

			Array.push_back( std::move( Group ) );
			return true;
		}
	);

	crow::json::wvalue Data;
	Data["count"] = Array.size();
	Data["groups"] = std::move( Array );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleGroupCreate( const crow::request& req, crow::response& res )
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

	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	std::string DisplayNameW( DisplayName.begin(), DisplayName.end() );
	std::string DescriptionW( Description.begin(), Description.end() );

	SQLiteDatabaseQueryInstance CreateGroup( m_GlobalContext->Database, "CreateGroup" );
	CreateGroup->Bind( "@DisplayName", DisplayNameW.c_str() );
	CreateGroup->Bind( "@Description", DescriptionW.c_str() );

	if( CreateGroup->Execute( nullptr ) < 0 )
	{
		crow::json::wvalue Data;
		Data["errorMessage"] = CreateGroup->GetLastError();
		res.set_header( "Content-Type", "application/json" );
		res.body = Data.dump();
		res.code = 400;
		res.end();
		return;
	}

	int64_t RowResult = CreateGroup->GetLastInsertionId();

	crow::json::wvalue Data;
	Data["id"] = RowResult;

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = RowResult > 0 ? 200 : 400;
	res.end();
}

void CrowListener::HandleGroupUpdate( const crow::request& req, crow::response& res )
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

	int GroupUID = body.has("id") ? (int)body["id"].i() : 0;
	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	std::string DisplayNameW( DisplayName.begin(), DisplayName.end() );
	std::string DescriptionW( Description.begin(), Description.end() );

	SQLiteDatabaseQueryInstance UpdateGroup( m_GlobalContext->Database, "UpdateGroup" );
	UpdateGroup->Bind( "@GroupUID", GroupUID );
	UpdateGroup->Bind( "@DisplayName", DisplayNameW.c_str() );
	UpdateGroup->Bind( "@Description", DescriptionW.c_str() );
	UpdateGroup->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleGroupDelete( const crow::request& req, crow::response& res )
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

	int GroupUID = body.has("id") ? (int)body["id"].i() : 0;

	SQLiteDatabaseQueryInstance DeleteGroup( m_GlobalContext->Database, "DeleteGroup" );
	DeleteGroup->Bind( "@GroupUID", GroupUID );
	DeleteGroup->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}