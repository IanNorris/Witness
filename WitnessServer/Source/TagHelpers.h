#pragma once

#include <string>
#include <vector>
#include <memory>

class SQLiteDatabase;

namespace TagHelpers
{
	struct TagInfo
	{
		int TagUID = 0;
		std::string Name;
		std::string Display;
		std::string Icon;
	};

	// Built-in icon map for known YOLO classes
	std::string GetDefaultIcon( const std::string& className );

	// Title-case a class name for display (e.g. "traffic light" -> "Traffic Light")
	std::string TitleCase( const std::string& name );

	// Find or create a tag by internal name, returns TagUID
	int FindOrCreateTag( const std::shared_ptr<SQLiteDatabase>& DB, const std::string& name );

	// Write ClipTag entries for a clip, given a semicolon/comma-delimited tag string
	void SyncClipTags( const std::shared_ptr<SQLiteDatabase>& DB, int64_t clipUID, const std::string& tagString );

	// Parse a tag string into individual trimmed tag names
	std::vector<std::string> ParseTagString( const std::string& tags );

	// Run the one-time migration: parse legacy Tags column -> populate Tag + ClipTag tables
	void MigrateLegacyTags( const std::shared_ptr<SQLiteDatabase>& DB );
}
