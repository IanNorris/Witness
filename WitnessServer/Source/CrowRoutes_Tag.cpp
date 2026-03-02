#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "TagHelpers.h"

// ===== Tag Handlers =====

void CrowListener::HandleTagEnum( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	std::vector<crow::json::wvalue> tags;

	SQLiteDatabaseQueryInstance SelectAllTags( m_GlobalContext->Database, "SelectAllTags" );
	SelectAllTags->Execute( [&tags]( const SQLiteDatabaseQuery& query )
	{
		crow::json::wvalue tag;
		tag["id"] = query.GetColumnValueInt( 0 );
		tag["name"] = std::string( query.GetColumnValueText( 1 ) ? query.GetColumnValueText( 1 ) : "" );
		tag["display"] = std::string( query.GetColumnValueText( 2 ) ? query.GetColumnValueText( 2 ) : "" );
		tag["icon"] = std::string( query.GetColumnValueText( 3 ) ? query.GetColumnValueText( 3 ) : "" );
		tag["sortOrder"] = query.GetColumnValueInt( 4 );
		tag["hidden"] = query.GetColumnValueInt( 5 );
		tag["clipCount"] = query.GetColumnValueInt( 6 );
		tags.push_back( std::move( tag ) );
		return true;
	});

	crow::json::wvalue Data;
	Data["tags"] = std::move( tags );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleTagUpdate( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	auto body = crow::json::load( req.body );
	if( !body || !body.has("id") )
	{
		res.code = 400;
		res.end();
		return;
	}

	int tagId = (int)body["id"].i();
	std::string display = body.has("display") ? std::string(body["display"].s()) : "";
	std::string icon = body.has("icon") ? std::string(body["icon"].s()) : "";
	int hidden = body.has("hidden") ? (int)body["hidden"].i() : 0;

	SQLiteDatabaseQueryInstance UpdateTag( m_GlobalContext->Database, "UpdateTag" );
	UpdateTag->Bind( "@TagUID", tagId );
	UpdateTag->Bind( "@Display", display.c_str() );
	UpdateTag->Bind( "@Icon", icon.c_str() );
	UpdateTag->Bind( "@Hidden", hidden );
	UpdateTag->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraTagExclusions( const crow::request& req, crow::response& res, int cameraId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	if( req.method == crow::HTTPMethod::GET )
	{
		std::vector<crow::json::wvalue> exclusions;
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectCameraTagExclusions" );
		q->Bind( "@CameraID", cameraId );
		q->Execute( [&exclusions]( const SQLiteDatabaseQuery& query )
		{
			exclusions.push_back( crow::json::wvalue( query.GetColumnValueInt( 0 ) ) );
			return true;
		});

		crow::json::wvalue Data;
		Data["cameraId"] = cameraId;
		Data["excludedTagIds"] = std::move( exclusions );

		res.set_header( "Content-Type", "application/json" );
		res.body = Data.dump();
		res.code = 200;
		res.end();
	}
	else
	{
		// POST: set exclusions
		auto body = crow::json::load( req.body );
		if( !body || !body.has("tagIds") )
		{
			res.code = 400;
			res.end();
			return;
		}

		// Clear existing
		{
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "DeleteCameraTagExclusions" );
			q->Bind( "@CameraID", cameraId );
			q->Execute( nullptr );
		}

		// Insert new
		for( size_t i = 0; i < body["tagIds"].size(); i++ )
		{
			int tagId = (int)body["tagIds"][i].i();
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "InsertCameraTagExclusion" );
			q->Bind( "@CameraID", cameraId );
			q->Bind( "@TagUID", tagId );
			q->Execute( nullptr );
		}

		res.set_header( "Content-Type", "application/json" );
		res.body = "{}";
		res.code = 200;
		res.end();
	}
}

void CrowListener::HandleClipReview( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( body.has("all") && body["all"].b() )
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SetAllClipsReviewed" );
		q->Execute( nullptr );
	}
	else if( body.has("ids") )
	{
		for( size_t i = 0; i < body["ids"].size(); i++ )
		{
			int clipId = (int)body["ids"][i].i();
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SetClipReviewed" );
			q->Bind( "@ClipUID", clipId );
			q->Bind( "@Reviewed", 1 );
			q->Execute( nullptr );
		}
	}
	else if( body.has("id") )
	{
		int clipId = (int)body["id"].i();
		int reviewed = body.has("value") ? (body["value"].b() ? 1 : 0) : 1;
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SetClipReviewed" );
		q->Bind( "@ClipUID", clipId );
		q->Bind( "@Reviewed", reviewed );
		q->Execute( nullptr );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipRecent( const crow::request& req, crow::response& res, int maxCount )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	maxCount = std::min( maxCount, 50 );

	std::vector<crow::json::wvalue> clips;
	std::vector<int64_t> clipUIDs;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectRecentUnreviewed" );
	q->Bind( "@UserUID", UserUID );
	q->Bind( "@MaxCount", maxCount );

	q->Execute( [&clips, &clipUIDs]( const SQLiteDatabaseQuery& query )
	{
		int64_t ClipID = query.GetColumnValueInt64( 0 );
		crow::json::wvalue Clip;
		Clip["clipUID"] = ClipID;
		Clip["timestamp"] = query.GetColumnValueInt64( 1 );
		Clip["cameraID"] = query.GetColumnValueInt( 2 );
		Clip["duration"] = query.GetColumnValueInt( 5 );
		Clip["recordMode"] = query.GetColumnValueInt( 6 );
		Clip["lighting"] = query.GetColumnValueInt( 12 );
		Clip["reviewed"] = query.GetColumnValueInt( 13 );

		clipUIDs.push_back( ClipID );
		clips.push_back( std::move( Clip ) );
		return true;
	});

	// Populate tags from ClipTag junction table
	for( size_t i = 0; i < clipUIDs.size(); i++ )
	{
		std::string tags;
		{
			SQLiteDatabaseQueryInstance tq( m_GlobalContext->Database, "SelectTagsForClip" );
			tq->Bind( "@ClipUID", clipUIDs[i] );
			tq->Execute( [&tags]( const SQLiteDatabaseQuery& query )
			{
				const char* name = query.GetColumnValueText( 1 );
				if( name )
				{
					if( !tags.empty() ) tags += ";";
					tags += name;
				}
				return true;
			});
		}
		clips[i]["tags"] = tags;
	}

	crow::json::wvalue Data;
	Data["clips"] = std::move( clips );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleClipCalendar( const crow::request& req, crow::response& res, int year, int month )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	// Calculate timestamp range for the month
	struct tm startTm = {};
	startTm.tm_year = year - 1900;
	startTm.tm_mon = month - 1;
	startTm.tm_mday = 1;
	time_t startTime = mktime( &startTm );

	struct tm endTm = startTm;
	endTm.tm_mon += 1;
	time_t endTime = mktime( &endTm );

	std::vector<crow::json::wvalue> days;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectClipCountsByDay" );
	q->Bind( "@TimestampFrom", (int64_t)startTime );
	q->Bind( "@TimestampTo", (int64_t)endTime );

	q->Execute( [&days]( const SQLiteDatabaseQuery& query )
	{
		crow::json::wvalue day;
		day["date"] = std::string( query.GetColumnValueText( 0 ) ? query.GetColumnValueText( 0 ) : "" );
		day["count"] = query.GetColumnValueInt( 1 );
		days.push_back( std::move( day ) );
		return true;
	});

	crow::json::wvalue Data;
	Data["year"] = year;
	Data["month"] = month;
	Data["days"] = std::move( days );

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}
