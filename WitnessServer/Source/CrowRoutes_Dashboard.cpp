#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "EventBroadcaster.h"
#include "DashboardDocument.h"
#include "DashboardStorage.h"

#include <Log.h>

#include <ctime>
#include <mutex>

// ===== Dashboard Handlers (dashboards v2, see DASHBOARDS_V2.md) =====
//
// Never answer 403 here: the web client treats 401/403 as "signed out" and
// would throw an unattended wall display back to the login page. A dashboard
// the caller may not see is reported as 404.

namespace
{
	// The dashboard tables share the main connection with the rest of the
	// server; serialise multi-statement operations (update + revision snapshot).
	std::mutex DashboardMutex;

	void Json( crow::response& res, int code, const std::string& body )
	{
		res.code = code;
		res.set_header( "Content-Type", "application/json" );
		res.body = body;
		res.end();
	}

	void Error( crow::response& res, int code, const char* error )
	{
		crow::json::wvalue data;
		data["error"] = error;
		Json( res, code, data.dump() );
	}

	const char* ScopeName( Dashboard::Scope scope )
	{
		return scope == Dashboard::Scope::Shared ? "shared" : "personal";
	}

	std::string TrimName( const std::string& value )
	{
		const auto first = value.find_first_not_of( " \t\r\n" );
		if( first == std::string::npos ) return {};
		const auto last = value.find_last_not_of( " \t\r\n" );
		return value.substr( first, last - first + 1 );
	}

	crow::json::wvalue SharesJson( const std::vector<Dashboard::Principal>& shares )
	{
		std::vector<crow::json::wvalue> list;
		for( const auto& principal : shares )
		{
			crow::json::wvalue item;
			item["type"] = principal.Type == Dashboard::PrincipalType::Everyone ? "everyone"
				: principal.Type == Dashboard::PrincipalType::User ? "user" : "group";
			item["id"] = principal.Id;
			list.push_back( std::move( item ) );
		}
		return crow::json::wvalue( std::move( list ) );
	}

	// Parses a stored canonical document, filters it for the viewer and
	// returns it together with the number of redacted references.
	bool LoadFiltered( const std::string& stored, const std::set<int>& accessible, Dashboard::Document& doc, int& redacted )
	{
		auto parsed = Dashboard::Parse( stored, true );
		if( !parsed.Ok ) return false;
		doc = std::move( parsed.Doc );
		redacted = Dashboard::FilterForViewer( doc, accessible );
		return true;
	}

	crow::json::wvalue Summary( sqlite3* db, const Dashboard::Row& row, int userUID, bool admin, const std::set<int>& accessible )
	{
		crow::json::wvalue item;
		item["id"] = row.Id;
		item["name"] = row.Name;
		item["scope"] = ScopeName( row.DashboardScope );
		item["kind"] = "canvas";
		item["revision"] = row.Revision;
		item["updatedAt"] = row.UpdatedAt;
		item["owned"] = row.OwnerUserUID == userUID;
		item["canEdit"] = Dashboard::CanEdit( row, userUID, admin );
		Dashboard::Document doc;
		int redacted = 0;
		if( LoadFiltered( row.Document, accessible, doc, redacted ) )
		{
			item["tiles"] = static_cast<int>( doc.Tiles.size() );
			item["redacted"] = redacted;
			std::vector<crow::json::wvalue> cameras;
			for( int id : Dashboard::ReadingOrderCameras( doc ) ) cameras.emplace_back( id );
			item["cameras"] = crow::json::wvalue( std::move( cameras ) );
		}
		(void)db;
		return item;
	}

	struct Caller
	{
		int UserUID = -1;
		bool Admin = false;
		std::set<int> Accessible;
	};

	bool Authenticate( GlobalContext& context, const crow::request& req, const crow::json::rvalue* body, Caller& caller )
	{
		caller.UserUID = CrowAuth::IsAuthenticated( context, req, body,
			body ? CrowAuth::Action::ReadWrite : CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
		if( caller.UserUID < 0 ) return false;
		caller.Admin = CrowAuth::IsAdministrator( context, caller.UserUID );
		caller.Accessible = CrowAuth::AccessibleCameraIds( context, caller.UserUID, true );
		return true;
	}

	void Changed( GlobalContext& context, int64_t id, int revision, bool deleted )
	{
		crow::json::wvalue ev;
		ev["id"] = id;
		ev["revision"] = revision;
		ev["deleted"] = deleted;
		context.Events->Broadcast( "dashboard:changed", std::move( ev ) );
	}
}

void CrowListener::HandleDashboardEnum( const crow::request& req, crow::response& res )
{
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, nullptr, caller ) ) return Error( res, 401, "unauthenticated" );
	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		std::vector<crow::json::wvalue> list;
		for( const auto& row : Dashboard::ListVisible( db, caller.UserUID, caller.Admin ) )
			list.push_back( Summary( db, row, caller.UserUID, caller.Admin, caller.Accessible ) );
		crow::json::wvalue data;
		data["dashboards"] = crow::json::wvalue( std::move( list ) );
		data["admin"] = caller.Admin;
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard enum failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardGet( const crow::request& req, crow::response& res, int id )
{
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, nullptr, caller ) ) return Error( res, 401, "unauthenticated" );
	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || !Dashboard::CanView( db, *row, caller.UserUID, caller.Admin ) ) return Error( res, 404, "not-found" );

		Dashboard::Document doc;
		int redacted = 0;
		if( !LoadFiltered( row->Document, caller.Accessible, doc, redacted ) ) return Error( res, 500, "corrupt-document" );

		crow::json::wvalue data = Summary( db, *row, caller.UserUID, caller.Admin, caller.Accessible );
		data["document"] = crow::json::wvalue( crow::json::load( Dashboard::Emit( doc ) ) );
		if( row->DashboardScope == Dashboard::Scope::Shared && caller.Admin )
			data["shares"] = SharesJson( Dashboard::GetShares( db, id ) );
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard get failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardCreate( const crow::request& req, crow::response& res )
{
	if( req.body.size() > Dashboard::MaxDocumentBytes + 4096 ) return Error( res, 413, "too-large" );
	auto body = crow::json::load( req.body );
	if( !body ) return Error( res, 400, "invalid-json" );
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, &body, caller ) ) return Error( res, 401, "unauthenticated" );

	const std::string name = TrimName( body.has( "name" ) && body["name"].t() == crow::json::type::String ? std::string( body["name"].s() ) : "" );
	if( name.empty() || name.size() > Dashboard::MaxNameLength ) return Error( res, 422, "invalid-name" );
	const bool shared = body.has( "scope" ) && body["scope"].t() == crow::json::type::String && std::string( body["scope"].s() ) == "shared";
	if( shared && !caller.Admin ) return Error( res, 422, "shared-requires-admin" );

	const std::string documentJson = body.has( "document" ) ? crow::json::wvalue( body["document"] ).dump() : "{}";
	auto parsed = Dashboard::Parse( documentJson, false );
	if( !parsed.Ok ) return Json( res, documentJson.size() > Dashboard::MaxDocumentBytes ? 413 : 422, Dashboard::IssuesJson( parsed.Issues ) );
	auto issues = Dashboard::Validate( parsed.Doc, caller.Accessible );
	if( !issues.empty() ) return Json( res, 422, Dashboard::IssuesJson( issues ) );

	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		if( !shared && Dashboard::CountOwnedPersonal( db, caller.UserUID ) >= Dashboard::MaxDashboardsPerUser )
			return Error( res, 422, "too-many-dashboards" );
		const int64_t id = Dashboard::Create( db, caller.UserUID, shared ? Dashboard::Scope::Shared : Dashboard::Scope::Personal,
			name, Dashboard::Emit( parsed.Doc ), static_cast<int64_t>( std::time( nullptr ) ) );
		Changed( *m_GlobalContext, id, 1, false );
		crow::json::wvalue data;
		data["id"] = id;
		data["revision"] = 1;
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard create failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardUpdate( const crow::request& req, crow::response& res )
{
	if( req.body.size() > Dashboard::MaxDocumentBytes + 4096 ) return Error( res, 413, "too-large" );
	auto body = crow::json::load( req.body );
	if( !body ) return Error( res, 400, "invalid-json" );
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, &body, caller ) ) return Error( res, 401, "unauthenticated" );
	if( !body.has( "id" ) || !body.has( "baseRevision" ) ) return Error( res, 400, "missing-fields" );
	const int64_t id = body["id"].i();
	const int baseRevision = static_cast<int>( body["baseRevision"].i() );

	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || !Dashboard::CanView( db, *row, caller.UserUID, caller.Admin ) ) return Error( res, 404, "not-found" );
		if( !Dashboard::CanEdit( *row, caller.UserUID, caller.Admin ) ) return Error( res, 422, "read-only" );
		if( row->Revision != baseRevision )
		{
			crow::json::wvalue data;
			data["error"] = "conflict";
			data["revision"] = row->Revision;
			return Json( res, 409, data.dump() );
		}

		std::string name = row->Name;
		if( body.has( "name" ) )
		{
			name = TrimName( body["name"].t() == crow::json::type::String ? std::string( body["name"].s() ) : "" );
			if( name.empty() || name.size() > Dashboard::MaxNameLength ) return Error( res, 422, "invalid-name" );
		}

		std::string canonical = row->Document;
		if( body.has( "document" ) )
		{
			const std::string documentJson = crow::json::wvalue( body["document"] ).dump();
			auto parsed = Dashboard::Parse( documentJson, true );
			if( !parsed.Ok ) return Json( res, documentJson.size() > Dashboard::MaxDocumentBytes ? 413 : 422, Dashboard::IssuesJson( parsed.Issues ) );
			auto stored = Dashboard::Parse( row->Document, true );
			// References the editor never saw are restored, and remain valid
			// even though the editor cannot access them.
			std::set<int> allowed = caller.Accessible;
			if( stored.Ok )
			{
				Dashboard::MergeRedacted( parsed.Doc, stored.Doc, caller.Accessible );
				for( int ref : Dashboard::ReferencedCameras( stored.Doc ) ) allowed.insert( ref );
			}
			auto issues = Dashboard::Validate( parsed.Doc, allowed );
			if( !issues.empty() ) return Json( res, 422, Dashboard::IssuesJson( issues ) );
			canonical = Dashboard::Emit( parsed.Doc );
		}

		int revision = 0;
		const auto result = Dashboard::Update( db, id, baseRevision, name, canonical, caller.UserUID,
			static_cast<int64_t>( std::time( nullptr ) ), &revision );
		if( result == Dashboard::UpdateResult::NotFound ) return Error( res, 404, "not-found" );
		if( result == Dashboard::UpdateResult::Conflict ) return Error( res, 409, "conflict" );
		Changed( *m_GlobalContext, id, revision, false );
		crow::json::wvalue data;
		data["id"] = id;
		data["revision"] = revision;
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard update failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) return Error( res, 400, "invalid-json" );
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, &body, caller ) ) return Error( res, 401, "unauthenticated" );
	if( !body.has( "id" ) ) return Error( res, 400, "missing-fields" );
	const int64_t id = body["id"].i();
	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || !Dashboard::CanView( db, *row, caller.UserUID, caller.Admin ) ) return Error( res, 404, "not-found" );
		if( !Dashboard::CanEdit( *row, caller.UserUID, caller.Admin ) ) return Error( res, 422, "read-only" );
		Dashboard::Delete( db, id );
		Changed( *m_GlobalContext, id, row->Revision, true );
		Json( res, 200, "{}" );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard delete failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardShare( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) return Error( res, 400, "invalid-json" );
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, &body, caller ) ) return Error( res, 401, "unauthenticated" );
	if( !body.has( "id" ) || !body.has( "principals" ) || body["principals"].t() != crow::json::type::List )
		return Error( res, 400, "missing-fields" );
	const int64_t id = body["id"].i();

	std::vector<Dashboard::Principal> principals;
	const auto& list = body["principals"];
	if( list.size() > 256 ) return Error( res, 422, "too-many-principals" );
	for( size_t i = 0; i < list.size(); ++i )
	{
		const auto& item = list[i];
		if( item.t() != crow::json::type::Object || !item.has( "type" ) ) return Error( res, 422, "invalid-principal" );
		const std::string type = item["type"].s();
		Dashboard::Principal principal;
		if( type == "everyone" ) principal.Type = Dashboard::PrincipalType::Everyone;
		else if( type == "user" ) principal.Type = Dashboard::PrincipalType::User;
		else if( type == "group" ) principal.Type = Dashboard::PrincipalType::Group;
		else return Error( res, 422, "invalid-principal" );
		if( principal.Type != Dashboard::PrincipalType::Everyone )
		{
			if( !item.has( "id" ) || item["id"].t() != crow::json::type::Number || item["id"].i() <= 0 ) return Error( res, 422, "invalid-principal" );
			principal.Id = static_cast<int>( item["id"].i() );
		}
		principals.push_back( principal );
	}

	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || !Dashboard::CanView( db, *row, caller.UserUID, caller.Admin ) ) return Error( res, 404, "not-found" );
		if( row->DashboardScope != Dashboard::Scope::Shared || !caller.Admin ) return Error( res, 422, "not-shareable" );
		Dashboard::SetShares( db, id, principals );
		Changed( *m_GlobalContext, id, row->Revision, false );
		crow::json::wvalue data;
		data["shares"] = SharesJson( Dashboard::GetShares( db, id ) );
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard share failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardHistory( const crow::request& req, crow::response& res, int id )
{
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, nullptr, caller ) ) return Error( res, 401, "unauthenticated" );
	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || !Dashboard::CanView( db, *row, caller.UserUID, caller.Admin ) || !Dashboard::CanEdit( *row, caller.UserUID, caller.Admin ) )
			return Error( res, 404, "not-found" );
		std::vector<crow::json::wvalue> list;
		for( const auto& revision : Dashboard::ListRevisions( db, id ) )
		{
			crow::json::wvalue item;
			item["revision"] = revision.Revision;
			item["updatedAt"] = revision.UpdatedAt;
			item["updatedBy"] = revision.UpdatedByUserUID;
			item["name"] = revision.Name;
			list.push_back( std::move( item ) );
		}
		crow::json::wvalue data;
		data["revisions"] = crow::json::wvalue( std::move( list ) );
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard history failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardRevision( const crow::request& req, crow::response& res, int id, int revision )
{
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, nullptr, caller ) ) return Error( res, 401, "unauthenticated" );
	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || !Dashboard::CanView( db, *row, caller.UserUID, caller.Admin ) || !Dashboard::CanEdit( *row, caller.UserUID, caller.Admin ) )
			return Error( res, 404, "not-found" );
		auto stored = Dashboard::GetRevisionDocument( db, id, revision );
		if( !stored ) return Error( res, 404, "not-found" );
		Dashboard::Document doc;
		int redacted = 0;
		if( !LoadFiltered( *stored, caller.Accessible, doc, redacted ) ) return Error( res, 500, "corrupt-document" );
		crow::json::wvalue data;
		data["revision"] = revision;
		data["redacted"] = redacted;
		data["document"] = crow::json::wvalue( crow::json::load( Dashboard::Emit( doc ) ) );
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard revision failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}

void CrowListener::HandleDashboardAccessPreview( const crow::request& req, crow::response& res, int id )
{
	Caller caller;
	if( !Authenticate( *m_GlobalContext, req, nullptr, caller ) ) return Error( res, 401, "unauthenticated" );
	if( !caller.Admin ) return Error( res, 404, "not-found" );
	try
	{
		std::lock_guard<std::mutex> lock( DashboardMutex );
		sqlite3* db = m_GlobalContext->Database->GetDatabase();
		auto row = Dashboard::Get( db, id );
		if( !row || row->DashboardScope != Dashboard::Scope::Shared ) return Error( res, 404, "not-found" );

		struct Person { int Id; std::string Name; bool Admin; bool Enabled; };
		std::vector<Person> people;
		{
			SQLiteDatabaseQueryInstance FindUsers( m_GlobalContext->Database, "FindUsers" );
			FindUsers->Execute( [&]( const SQLiteDatabaseQuery& query )
			{
				people.push_back( { query.GetColumnValueInt( 0 ), query.GetColumnValueText( 2 ),
					query.GetColumnValueInt( 4 ) != 0, query.GetColumnValueInt( 3 ) != 0 } );
				return true;
			} );
		}

		std::vector<crow::json::wvalue> list;
		for( const auto& person : people )
		{
			if( !person.Enabled || !Dashboard::CanView( db, *row, person.Id, person.Admin ) ) continue;
			Dashboard::Document doc;
			int redacted = 0;
			LoadFiltered( row->Document, CrowAuth::AccessibleCameraIds( *m_GlobalContext, person.Id, true ), doc, redacted );
			crow::json::wvalue item;
			item["userId"] = person.Id;
			item["displayName"] = person.Name;
			item["redacted"] = redacted;
			list.push_back( std::move( item ) );
		}
		crow::json::wvalue data;
		data["viewers"] = crow::json::wvalue( std::move( list ) );
		Json( res, 200, data.dump() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "Dashboard access preview failed: %s", e.what() );
		Error( res, 500, "storage" );
	}
}
