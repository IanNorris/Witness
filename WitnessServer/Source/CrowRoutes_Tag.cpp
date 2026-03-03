#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "TagHelpers.h"

#include <Log.h>
#include <set>
#include <map>

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
	auto body = crow::json::load( req.body );
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

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

void CrowListener::HandleClipTimeline( const crow::request& req, crow::response& res, const std::string& fromStr, const std::string& toStr )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int64_t tsFrom = std::stoll( fromStr );
	int64_t tsTo = std::stoll( toStr );
	int64_t rangeSecs = tsTo - tsFrom;

	// Target ~288 slots, round bucket to nearest clean time unit
	int64_t rawBucket = rangeSecs / 288;
	int64_t bucketSecs;
	if( rawBucket <= 300 )        bucketSecs = 300;       // 5 min
	else if( rawBucket <= 900 )   bucketSecs = 900;       // 15 min
	else if( rawBucket <= 1800 )  bucketSecs = 1800;      // 30 min
	else if( rawBucket <= 3600 )  bucketSecs = 3600;      // 1 hour
	else if( rawBucket <= 7200 )  bucketSecs = 7200;      // 2 hours
	else if( rawBucket <= 21600 ) bucketSecs = 21600;     // 6 hours
	else if( rawBucket <= 86400 ) bucketSecs = 86400;     // 1 day
	else                          bucketSecs = 604800;    // 1 week

	// Minimum clip duration to include (filter short triggers for longer ranges)
	int minDuration = (rangeSecs > 86400) ? 3 : 0;

	// Raw clip data
	struct RawClip {
		int64_t uid;
		int64_t timestamp;
		int duration;
		int cameraID;
		int recordMode;
	};
	std::vector<RawClip> rawClips;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectClipsForTimeline" );
	q->Bind( "@TimestampFrom", tsFrom );
	q->Bind( "@TimestampTo", tsTo );

	q->Execute( [&rawClips, minDuration]( const SQLiteDatabaseQuery& query )
	{
		int duration = query.GetColumnValueInt( 2 );
		if( duration < minDuration ) return true; // skip short clips

		RawClip c;
		c.uid = query.GetColumnValueInt64( 0 );
		c.timestamp = query.GetColumnValueInt64( 1 );
		c.duration = duration;
		c.cameraID = query.GetColumnValueInt( 3 );
		c.recordMode = query.GetColumnValueInt( 5 );
		rawClips.push_back( c );
		return true;
	});

	// Fetch tags — limit to 500 clips max to avoid hammering the DB on huge ranges
	std::unordered_map<int64_t, std::vector<std::string>> clipTags;
	{
		size_t tagLimit = std::min( rawClips.size(), (size_t)500 );
		// Sample evenly across clips to get representative tags
		size_t step = rawClips.size() > tagLimit ? rawClips.size() / tagLimit : 1;
		for( size_t i = 0; i < rawClips.size(); i += step )
		{
			auto& c = rawClips[i];
			SQLiteDatabaseQueryInstance tq( m_GlobalContext->Database, "SelectTagsForClip" );
			tq->Bind( "@ClipUID", c.uid );
			tq->Execute( [&clipTags, uid = c.uid]( const SQLiteDatabaseQuery& query )
			{
				const char* name = query.GetColumnValueText( 1 );
				if( name )
					clipTags[uid].push_back( name );
				return true;
			});
		}
	}

	// Aggregate into time buckets
	struct Event {
		int64_t from;
		int64_t to;
		int clipCount = 0;
		std::set<int> cameraIDs;
		std::set<std::string> tags;
	};

	auto buildBuckets = [&]( int64_t bs ) -> std::map<int64_t, Event>
	{
		std::map<int64_t, Event> buckets;
		for( auto& c : rawClips )
		{
			int64_t bucketKey = ((c.timestamp - tsFrom) / bs) * bs + tsFrom;
			auto& evt = buckets[bucketKey];
			if( evt.clipCount == 0 )
			{
				evt.from = bucketKey;
				evt.to = bucketKey + bs;
			}
			evt.clipCount++;
			evt.cameraIDs.insert( c.cameraID );
			auto it = clipTags.find( c.uid );
			if( it != clipTags.end() )
			{
				for( auto& tag : it->second )
					evt.tags.insert( tag );
			}
		}
		return buckets;
	};

	// Re-bucket if too many events (cap at 1000)
	auto buckets = buildBuckets( bucketSecs );
	while( buckets.size() > 1000 && bucketSecs < rangeSecs )
	{
		bucketSecs *= 2;
		buckets = buildBuckets( bucketSecs );
	}

	// Build response
	std::vector<crow::json::wvalue> events;
	for( auto& [key, evt] : buckets )
	{
		crow::json::wvalue e;
		e["from"] = evt.from;
		e["to"] = evt.to;
		e["clipCount"] = evt.clipCount;

		std::vector<crow::json::wvalue> cams;
		for( int id : evt.cameraIDs )
			cams.push_back( crow::json::wvalue( id ) );
		e["cameraIDs"] = std::move( cams );

		// Join tags as semicolon-separated string
		std::string tagStr;
		for( auto& tag : evt.tags )
		{
			if( !tagStr.empty() ) tagStr += ";";
			tagStr += tag;
		}
		e["tags"] = tagStr;

		events.push_back( std::move( e ) );
	}

	crow::json::wvalue Data;
	Data["from"] = tsFrom;
	Data["to"] = tsTo;
	Data["bucketSeconds"] = bucketSecs;
	Data["events"] = std::move( events );

	// Include clip retention cutoff — always show if retention days configured
	{
		std::string retentionStr;
		SQLiteDatabaseQueryInstance rq( m_GlobalContext->Database, "GetSetting" );
		rq->Bind( "@Name", "clip_retention_days" );
		rq->Execute( [&retentionStr]( const SQLiteDatabaseQuery& query ) {
			const char* val = query.GetColumnValueText( 0 );
			if( val ) retentionStr = val;
			return true;
		});

		int retentionDays = 0;
		if( !retentionStr.empty() )
		{
			int parsed = std::atoi( retentionStr.c_str() );
			if( parsed > 0 ) retentionDays = parsed;
		}

		if( retentionDays > 0 )
		{
			int64_t cutoff = static_cast<int64_t>( time( nullptr ) ) - (int64_t)retentionDays * 86400;
			Data["retentionCutoff"] = cutoff;
			Data["retentionDays"] = retentionDays;
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = Data.dump();
	res.code = 200;
	res.end();
}
