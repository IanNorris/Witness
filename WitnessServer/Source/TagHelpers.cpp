#include "TagHelpers.h"
#include "SQLite.h"

#include <Log.h>

#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace TagHelpers
{
	static const std::map<std::string, std::string> s_IconMap = {
		{"person",     "\xF0\x9F\x99\x8D"},       // 🙍
		{"car",        "\xF0\x9F\x9A\x97"},        // 🚗
		{"truck",      "\xF0\x9F\x9A\x9B"},        // 🚛
		{"cat",        "\xF0\x9F\x90\xB1"},         // 🐱
		{"dog",        "\xF0\x9F\x90\x95"},         // 🐕
		{"bicycle",    "\xF0\x9F\x9A\xB2"},        // 🚲
		{"motorcycle", "\xF0\x9F\x8F\x8D\xEF\xB8\x8F"}, // 🏍️
		{"bird",       "\xF0\x9F\x90\xA6"},         // 🐦
		{"bus",        "\xF0\x9F\x9A\x8C"},        // 🚌
		{"boat",       "\xE2\x9B\xB5"},             // ⛵
		{"horse",      "\xF0\x9F\x90\xB4"},        // 🐴
		{"sheep",      "\xF0\x9F\x90\x91"},        // 🐑
		{"cow",        "\xF0\x9F\x90\x84"},         // 🐄
		{"bear",       "\xF0\x9F\x90\xBB"},         // 🐻
	};

	std::string GetDefaultIcon( const std::string& className )
	{
		auto it = s_IconMap.find( className );
		if( it != s_IconMap.end() )
			return it->second;
		return "";
	}

	std::string TitleCase( const std::string& name )
	{
		std::string result = name;
		bool nextUpper = true;
		for( size_t i = 0; i < result.size(); i++ )
		{
			if( result[i] == ' ' || result[i] == '_' )
			{
				if( result[i] == '_' )
					result[i] = ' ';
				nextUpper = true;
			}
			else if( nextUpper )
			{
				result[i] = (char)std::toupper( (unsigned char)result[i] );
				nextUpper = false;
			}
		}
		return result;
	}

	std::vector<std::string> ParseTagString( const std::string& tags )
	{
		std::vector<std::string> result;
		std::string current;
		for( char c : tags )
		{
			if( c == ';' || c == ',' )
			{
				// Trim
				size_t start = current.find_first_not_of( ' ' );
				size_t end = current.find_last_not_of( ' ' );
				if( start != std::string::npos )
					result.push_back( current.substr( start, end - start + 1 ) );
				current.clear();
			}
			else
			{
				current += c;
			}
		}
		// Last token
		size_t start = current.find_first_not_of( ' ' );
		size_t end = current.find_last_not_of( ' ' );
		if( start != std::string::npos )
			result.push_back( current.substr( start, end - start + 1 ) );

		return result;
	}

	int FindOrCreateTag( const std::shared_ptr<SQLiteDatabase>& DB, const std::string& name )
	{
		std::string display = TitleCase( name );
		std::string icon = GetDefaultIcon( name );

		// Insert if not exists
		{
			SQLiteDatabaseQueryInstance q( DB, "FindOrCreateTag" );
			q->Bind( "@Name", name.c_str() );
			q->Bind( "@Display", display.c_str() );
			q->Bind( "@Icon", icon.c_str() );
			q->Bind( "@SortOrder", 0 );
			int result = q->Execute( nullptr );
			if( result < 0 )
			{
				LOG_INFO( "FindOrCreateTag INSERT failed for '%s'", name.c_str() );
			}
		}

		// Get the UID
		int tagUID = 0;
		{
			SQLiteDatabaseQueryInstance q( DB, "SelectTagByName" );
			q->Bind( "@Name", name.c_str() );
			int rows = q->Execute( [&tagUID]( const SQLiteDatabaseQuery& query )
			{
				tagUID = query.GetColumnValueInt( 0 );
				return true;
			});
			if( rows == 0 )
			{
				LOG_INFO( "SelectTagByName found 0 rows for '%s'", name.c_str() );
			}
		}

		return tagUID;
	}

	void SyncClipTags( const std::shared_ptr<SQLiteDatabase>& DB, int64_t clipUID, const std::string& tagString )
	{
		// Delete existing clip-tag mappings
		{
			SQLiteDatabaseQueryInstance q( DB, "DeleteClipTags" );
			q->Bind( "@ClipUID", clipUID );
			q->Execute( nullptr );
		}

		// Parse and insert new ones
		auto tags = ParseTagString( tagString );
		for( const auto& tag : tags )
		{
			int tagUID = FindOrCreateTag( DB, tag );
			if( tagUID > 0 )
			{
				SQLiteDatabaseQueryInstance q( DB, "InsertClipTag" );
				q->Bind( "@ClipUID", clipUID );
				q->Bind( "@TagUID", tagUID );
				q->Execute( nullptr );
			}
		}
	}

	void MigrateLegacyTags( const std::shared_ptr<SQLiteDatabase>& DB )
	{
		// Check if migration is needed: if ClipTag table has any rows, skip
		int existingCount = 0;
		sqlite3_exec( DB->GetDatabase(), "SELECT COUNT(*) FROM ClipTag;",
			[]( void* data, int, char** argv, char** ) -> int
			{
				*(int*)data = std::atoi( argv[0] );
				return 0;
			},
			&existingCount, nullptr );

		if( existingCount > 0 )
			return; // Already migrated

		LOG_INFO( "Migrating legacy tags to Tag/ClipTag tables..." );

		// Read all clips with tags
		struct ClipTagRow { int64_t clipUID; std::string tags; };
		std::vector<ClipTagRow> rows;

		{
			SQLiteDatabaseQueryInstance q( DB, "SelectClipsWithTags" );
			q->Execute( [&rows]( const SQLiteDatabaseQuery& query )
			{
				ClipTagRow row;
				row.clipUID = query.GetColumnValueInt64( 0 );
				const char* t = query.GetColumnValueText( 1 );
				row.tags = t ? t : "";
				rows.push_back( std::move( row ) );
				return true;
			});
		}

		if( !rows.empty() )
		{
			LOG_INFO( "Sample tag data: clip %lld tags='%s'", (long long)rows[0].clipUID, rows[0].tags.c_str() );
		}

		// Process in a transaction for speed
		sqlite3_exec( DB->GetDatabase(), "BEGIN TRANSACTION;", nullptr, nullptr, nullptr );

		int tagCount = 0;
		int clipTagCount = 0;
		for( const auto& row : rows )
		{
			auto tags = ParseTagString( row.tags );
			if( tags.empty() && !row.tags.empty() )
			{
				LOG_INFO( "ParseTagString returned empty for clip %lld with tags='%s' (len=%d)",
					(long long)row.clipUID, row.tags.c_str(), (int)row.tags.size() );
			}
			for( const auto& tag : tags )
			{
				int tagUID = FindOrCreateTag( DB, tag );
				if( tagUID > 0 )
				{
					SQLiteDatabaseQueryInstance q( DB, "InsertClipTag" );
					q->Bind( "@ClipUID", row.clipUID );
					q->Bind( "@TagUID", tagUID );
					q->Execute( nullptr );
					clipTagCount++;
				}
			}
		}

		sqlite3_exec( DB->GetDatabase(), "COMMIT;", nullptr, nullptr, nullptr );

		// Count unique tags created
		sqlite3_exec( DB->GetDatabase(), "SELECT COUNT(*) FROM Tag;",
			[]( void* data, int, char** argv, char** ) -> int
			{
				*(int*)data = std::atoi( argv[0] );
				return 0;
			},
			&tagCount, nullptr );

		LOG_INFO( "Tag migration complete: %d unique tags, %d clip-tag mappings from %d clips",
			tagCount, clipTagCount, (int)rows.size() );
	}
}
