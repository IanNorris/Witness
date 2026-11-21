#pragma once

// Dashboard documents (schema 2): parsing, validation, canonical emission and
// per-viewer filtering. Pure: depends only on crow::json, so it is unit-tested
// without a server (Tools/Tests/DashboardDocumentTests.cpp).
//
// Stored documents are always canonical JSON re-emitted from the parsed model,
// so unknown fields never reach the database. Every read is filtered against
// the viewer's *current* camera access; stored data is never trusted.

#include <set>
#include <string>
#include <vector>

namespace Dashboard
{
	constexpr int SchemaVersion = 2;
	constexpr size_t MaxDocumentBytes = 64 * 1024;
	constexpr size_t MaxTiles = 64;
	constexpr size_t MaxBadges = 8;
	constexpr size_t MaxSources = 64;
	constexpr size_t MaxNameLength = 80;
	constexpr int MaxDashboardsPerUser = 50;

	struct Tile
	{
		std::string Id;
		std::string Type;          // camera | spotlight | activity | clock | status | timeline | detection | ptz | text | unavailable
		int X = 0, Y = 0, W = 0, H = 0;

		// camera / ptz
		int Camera = 0;
		std::string Stream = "auto";  // auto | main | sub
		std::string Fit = "contain";  // contain | cover
		bool Label = true;
		bool Playback = true;

		// spotlight
		std::string Trigger = "motion";   // motion | rotate
		std::string SourcesKind = "list"; // list | all
		std::vector<int> Sources;         // ordered = priority
		bool BorrowPlaced = false;
		int HoldSeconds = 15;
		std::string IdleKind = "blank";   // camera | rotate | last | blank
		int IdleCamera = 0;
		int IdleEverySeconds = 20;
		bool Warm = true;
		int MinSplit = 1;                 // 1..4, at most MaxSplit: cells always filled
		int MaxSplit = 4;                 // 1..4 simultaneous cameras

		// activity / timeline / detection scope
		std::string ScopeKind = "dashboard"; // dashboard | all | list
		std::vector<int> ScopeCameras;
		std::string Direction = "auto";      // auto | horizontal | vertical
		bool Grouping = true;
		bool ObjectFocus = true;
		int Hours = 6;                       // timeline

		// clock
		bool Seconds = true;
		bool Date = true;

		// status
		std::string Variant = "summary";     // summary | health

		// text
		std::string Text;
		std::string Size = "m";              // s | m | l

		// unavailable (only produced by filtering)
		bool Redacted = false;
		std::string Reason;
	};

	struct Badge
	{
		std::string Id;
		std::string Type = "clock";
		std::string Anchor = "bottom-right";  // top-left | top-right | bottom-left | bottom-right
		std::string Size = "m";
	};

	struct Document
	{
		int Cols = 48, Rows = 48, Gap = 4, Radius = 8;
		int TargetWidth = 1920, TargetHeight = 1080;
		std::vector<Tile> Tiles;
		std::vector<Badge> Badges;
		std::vector<int> Priority;
		int ReturnToLiveSeconds = 180;
		bool QuietControls = true;
		// Full-quality streams: 0 = automatic decode budget, -1 = no limit,
		// 1..16 = at most that many (larger tiles first).
		int MainStreams = 0;
		std::vector<int> MobileOrder;    // optional phone ordering override
	};

	struct Issue
	{
		std::string Tile;   // tile id, or "" for document-level issues
		std::string Code;
	};

	struct ParseResult
	{
		bool Ok = false;
		Document Doc;
		std::vector<Issue> Issues;
	};

	// Parses JSON into the model. allowUnavailable admits filtered placeholder
	// tiles (they must then be resolved by MergeRedacted before validation).
	ParseResult Parse( const std::string& json, bool allowUnavailable );

	// Structural checks (bounds, overlap, ids, limits) and camera references.
	// Every referenced camera must be in allowedCameras. Missing and
	// inaccessible cameras produce the same code so ids cannot be probed.
	std::vector<Issue> Validate( const Document& doc, const std::set<int>& allowedCameras );

	// Restores references the editor could not see: placeholder tiles take
	// back their stored tile (keeping the new rectangle), and hidden sources,
	// scopes, priorities and idle cameras are re-appended from the stored copy.
	void MergeRedacted( Document& incoming, const Document& stored, const std::set<int>& editorAccess );

	// Removes everything the viewer cannot access. Inaccessible camera tiles
	// become "unavailable" placeholders without any camera id or label.
	// Returns the number of redacted references.
	int FilterForViewer( Document& doc, const std::set<int>& accessible );

	// All camera ids referenced anywhere in the document.
	std::set<int> ReferencedCameras( const Document& doc );

	// Camera ids in reading order (top-to-bottom, left-to-right), honouring
	// MobileOrder first. Used for the phone list view.
	std::vector<int> ReadingOrderCameras( const Document& doc );

	std::string Emit( const Document& doc );
	std::string IssuesJson( const std::vector<Issue>& issues );
}
