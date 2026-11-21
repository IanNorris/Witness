#include "DashboardDocument.h"

#include <crow/json.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>

namespace Dashboard
{
namespace
{
	using crow::json::rvalue;
	using crow::json::type;
	using crow::json::wvalue;

	const std::set<std::string> CanvasTileTypes = {
		"camera", "spotlight", "activity", "clock", "status", "timeline", "detection", "ptz", "text" };

	struct Reader
	{
		std::vector<Issue>& Issues;
		std::string TileId;

		void Fail( const std::string& code ) { Issues.push_back( { TileId, code } ); }

		bool IsInteger( const rvalue& v )
		{
			if( v.t() != type::Number ) return false;
			const double d = v.d();
			return std::isfinite( d ) && std::floor( d ) == d && std::abs( d ) < 1e9;
		}

		void Int( const rvalue& obj, const char* key, int& out, int lo, int hi )
		{
			if( !obj.has( key ) ) return;
			const rvalue& v = obj[key];
			if( !IsInteger( v ) ) { Fail( std::string( "invalid-" ) + key ); return; }
			const int value = static_cast<int>( v.d() );
			if( value < lo || value > hi ) { Fail( std::string( "out-of-range-" ) + key ); return; }
			out = value;
		}

		void Bool( const rvalue& obj, const char* key, bool& out )
		{
			if( !obj.has( key ) ) return;
			const rvalue& v = obj[key];
			if( v.t() == type::True ) out = true;
			else if( v.t() == type::False ) out = false;
			else Fail( std::string( "invalid-" ) + key );
		}

		void Enum( const rvalue& obj, const char* key, std::string& out, std::initializer_list<const char*> allowed )
		{
			if( !obj.has( key ) ) return;
			const rvalue& v = obj[key];
			if( v.t() != type::String ) { Fail( std::string( "invalid-" ) + key ); return; }
			const std::string value = v.s();
			for( const char* candidate : allowed )
				if( value == candidate ) { out = value; return; }
			Fail( std::string( "invalid-" ) + key );
		}

		void Text( const rvalue& obj, const char* key, std::string& out, size_t maxLength )
		{
			if( !obj.has( key ) ) return;
			const rvalue& v = obj[key];
			if( v.t() != type::String ) { Fail( std::string( "invalid-" ) + key ); return; }
			std::string value = v.s();
			if( value.size() > maxLength ) { Fail( std::string( "too-long-" ) + key ); return; }
			out = std::move( value );
		}

		void CameraList( const rvalue& obj, const char* key, std::vector<int>& out )
		{
			if( !obj.has( key ) ) return;
			const rvalue& v = obj[key];
			if( v.t() != type::List ) { Fail( std::string( "invalid-" ) + key ); return; }
			if( v.size() > MaxSources ) { Fail( std::string( "too-many-" ) + key ); return; }
			out.clear();
			std::set<int> seen;
			for( size_t i = 0; i < v.size(); ++i )
			{
				if( !IsInteger( v[i] ) || v[i].d() <= 0 ) { Fail( std::string( "invalid-" ) + key ); return; }
				const int id = static_cast<int>( v[i].d() );
				if( seen.insert( id ).second ) out.push_back( id );
			}
		}
	};

	wvalue IntList( const std::vector<int>& values )
	{
		std::vector<wvalue> list;
		for( int v : values ) list.emplace_back( v );
		return wvalue( std::move( list ) );
	}

	bool Overlaps( const Tile& a, const Tile& b )
	{
		return a.X < b.X + b.W && b.X < a.X + a.W && a.Y < b.Y + b.H && b.Y < a.Y + a.H;
	}

	void AppendMissing( std::vector<int>& target, const std::vector<int>& source, const std::set<int>& hiddenOnly )
	{
		for( int id : source )
			if( !hiddenOnly.count( id ) && std::find( target.begin(), target.end(), id ) == target.end() )
				target.push_back( id );
	}

	// Ids in `stored` the editor cannot access (and therefore never saw).
	std::vector<int> Hidden( const std::vector<int>& stored, const std::set<int>& editorAccess )
	{
		std::vector<int> result;
		for( int id : stored )
			if( !editorAccess.count( id ) ) result.push_back( id );
		return result;
	}

	void Retain( std::vector<int>& ids, const std::set<int>& accessible, int& redacted )
	{
		const auto before = ids.size();
		ids.erase( std::remove_if( ids.begin(), ids.end(), [&]( int id ) { return !accessible.count( id ); } ), ids.end() );
		redacted += static_cast<int>( before - ids.size() );
	}

	void MakeUnavailable( Tile& tile )
	{
		Tile placeholder;
		placeholder.Id = tile.Id;
		placeholder.Type = "unavailable";
		placeholder.X = tile.X; placeholder.Y = tile.Y; placeholder.W = tile.W; placeholder.H = tile.H;
		placeholder.Redacted = true;
		placeholder.Reason = "no-access";
		tile = std::move( placeholder );
	}

	bool UsesScope( const std::string& type )
	{
		return type == "activity" || type == "timeline" || type == "detection";
	}
}

ParseResult Parse( const std::string& json, bool allowUnavailable )
{
	ParseResult result;
	if( json.size() > MaxDocumentBytes ) { result.Issues.push_back( { "", "too-large" } ); return result; }

	const rvalue root = crow::json::load( json );
	if( !root || root.t() != type::Object ) { result.Issues.push_back( { "", "invalid-json" } ); return result; }

	Reader read{ result.Issues, "" };
	Document& doc = result.Doc;

	if( root.has( "schema" ) && ( !read.IsInteger( root["schema"] ) || static_cast<int>( root["schema"].d() ) != SchemaVersion ) )
		read.Fail( "unsupported-schema" );

	if( root.has( "grid" ) )
	{
		const rvalue& grid = root["grid"];
		if( grid.t() != type::Object ) read.Fail( "invalid-grid" );
		else
		{
			read.Int( grid, "cols", doc.Cols, 4, 96 );
			read.Int( grid, "rows", doc.Rows, 4, 96 );
			read.Int( grid, "gap", doc.Gap, 0, 16 );
			read.Int( grid, "radius", doc.Radius, 0, 24 );
		}
	}
	if( root.has( "target" ) )
	{
		const rvalue& target = root["target"];
		if( target.t() != type::Object ) read.Fail( "invalid-target" );
		else
		{
			read.Int( target, "width", doc.TargetWidth, 320, 7680 );
			read.Int( target, "height", doc.TargetHeight, 320, 4320 );
		}
	}
	if( root.has( "behavior" ) )
	{
		const rvalue& behavior = root["behavior"];
		if( behavior.t() != type::Object ) read.Fail( "invalid-behavior" );
		else
		{
			read.CameraList( behavior, "priority", doc.Priority );
			read.Int( behavior, "returnToLiveSeconds", doc.ReturnToLiveSeconds, 0, 3600 );
			read.Bool( behavior, "quietControls", doc.QuietControls );
			read.Int( behavior, "mainStreams", doc.MainStreams, -1, 16 );
		}
	}
	read.CameraList( root, "mobileOrder", doc.MobileOrder );

	if( root.has( "tiles" ) )
	{
		const rvalue& tiles = root["tiles"];
		if( tiles.t() != type::List ) read.Fail( "invalid-tiles" );
		else if( tiles.size() > MaxTiles ) read.Fail( "too-many-tiles" );
		else for( size_t i = 0; i < tiles.size(); ++i )
		{
			const rvalue& t = tiles[i];
			if( t.t() != type::Object ) { read.Fail( "invalid-tile" ); continue; }
			Tile tile;
			read.TileId = "";
			read.Text( t, "id", tile.Id, 12 );
			read.TileId = tile.Id;
			read.Text( t, "type", tile.Type, 16 );
			const bool known = CanvasTileTypes.count( tile.Type ) || ( allowUnavailable && tile.Type == "unavailable" );
			if( !known ) { read.Fail( "unknown-tile-type" ); continue; }
			const int maxCoordinate = 96;
			if( !t.has( "x" ) || !t.has( "y" ) || !t.has( "w" ) || !t.has( "h" ) ) { read.Fail( "missing-rect" ); continue; }
			read.Int( t, "x", tile.X, 0, maxCoordinate );
			read.Int( t, "y", tile.Y, 0, maxCoordinate );
			read.Int( t, "w", tile.W, 1, maxCoordinate );
			read.Int( t, "h", tile.H, 1, maxCoordinate );

			if( tile.Type == "camera" || tile.Type == "ptz" )
			{
				read.Int( t, "camera", tile.Camera, 1, 1000000000 );
				if( tile.Camera <= 0 ) read.Fail( "missing-camera" );
				read.Bool( t, "label", tile.Label );
				if( tile.Type == "camera" )
				{
					read.Enum( t, "stream", tile.Stream, { "auto", "main", "sub" } );
					read.Enum( t, "fit", tile.Fit, { "contain", "cover" } );
					read.Bool( t, "playback", tile.Playback );
				}
			}
			else if( tile.Type == "spotlight" )
			{
				read.Enum( t, "trigger", tile.Trigger, { "motion", "rotate" } );
				if( t.has( "sources" ) )
				{
					const rvalue& sources = t["sources"];
					if( sources.t() != type::Object ) read.Fail( "invalid-sources" );
					else
					{
						read.Enum( sources, "kind", tile.SourcesKind, { "list", "all" } );
						read.CameraList( sources, "cameras", tile.Sources );
					}
				}
				read.Bool( t, "borrowPlaced", tile.BorrowPlaced );
				read.Int( t, "holdSeconds", tile.HoldSeconds, 3, 120 );
				if( t.has( "idle" ) )
				{
					const rvalue& idle = t["idle"];
					if( idle.t() != type::Object ) read.Fail( "invalid-idle" );
					else
					{
						read.Enum( idle, "kind", tile.IdleKind, { "camera", "rotate", "last", "blank" } );
						read.Int( idle, "camera", tile.IdleCamera, 1, 1000000000 );
						read.Int( idle, "everySeconds", tile.IdleEverySeconds, 5, 600 );
						if( tile.IdleKind == "camera" && tile.IdleCamera <= 0 ) read.Fail( "missing-idle-camera" );
						if( tile.IdleKind != "camera" ) tile.IdleCamera = 0;
					}
				}
				read.Bool( t, "warm", tile.Warm );
				read.Int( t, "maxSplit", tile.MaxSplit, 1, 4 );
				read.Int( t, "minSplit", tile.MinSplit, 1, 4 );
				tile.MinSplit = std::min( tile.MinSplit, tile.MaxSplit );
				read.Bool( t, "label", tile.Label );
			}
			else if( UsesScope( tile.Type ) )
			{
				if( t.has( "scope" ) )
				{
					const rvalue& scope = t["scope"];
					if( scope.t() != type::Object ) read.Fail( "invalid-scope" );
					else
					{
						read.Enum( scope, "kind", tile.ScopeKind, { "dashboard", "all", "list" } );
						read.CameraList( scope, "cameras", tile.ScopeCameras );
						if( tile.ScopeKind != "list" ) tile.ScopeCameras.clear();
					}
				}
				read.Enum( t, "direction", tile.Direction, { "auto", "horizontal", "vertical" } );
				read.Bool( t, "grouping", tile.Grouping );
				read.Bool( t, "objectFocus", tile.ObjectFocus );
				read.Int( t, "hours", tile.Hours, 1, 48 );
			}
			else if( tile.Type == "clock" )
			{
				read.Bool( t, "seconds", tile.Seconds );
				read.Bool( t, "date", tile.Date );
			}
			else if( tile.Type == "status" )
			{
				read.Enum( t, "variant", tile.Variant, { "summary", "health" } );
			}
			else if( tile.Type == "text" )
			{
				read.Text( t, "text", tile.Text, 200 );
				read.Enum( t, "size", tile.Size, { "s", "m", "l" } );
			}
			else if( tile.Type == "unavailable" )
			{
				tile.Redacted = true;
				tile.Reason = "no-access";
			}
			doc.Tiles.push_back( std::move( tile ) );
		}
	}
	read.TileId = "";

	if( root.has( "badges" ) )
	{
		const rvalue& badges = root["badges"];
		if( badges.t() != type::List ) read.Fail( "invalid-badges" );
		else if( badges.size() > MaxBadges ) read.Fail( "too-many-badges" );
		else for( size_t i = 0; i < badges.size(); ++i )
		{
			const rvalue& b = badges[i];
			if( b.t() != type::Object ) { read.Fail( "invalid-badge" ); continue; }
			Badge badge;
			read.Text( b, "id", badge.Id, 12 );
			read.Enum( b, "type", badge.Type, { "clock" } );
			read.Enum( b, "anchor", badge.Anchor, { "top-left", "top-right", "bottom-left", "bottom-right" } );
			read.Enum( b, "size", badge.Size, { "s", "m", "l" } );
			doc.Badges.push_back( std::move( badge ) );
		}
	}

	result.Ok = result.Issues.empty();
	return result;
}

std::vector<Issue> Validate( const Document& doc, const std::set<int>& allowedCameras )
{
	std::vector<Issue> issues;
	static const std::regex IdPattern( "^[a-z0-9]{1,12}$" );
	std::set<std::string> ids;
	std::set<int> placedCameras;

	for( size_t i = 0; i < doc.Tiles.size(); ++i )
	{
		const Tile& tile = doc.Tiles[i];
		if( !std::regex_match( tile.Id, IdPattern ) ) issues.push_back( { tile.Id, "invalid-id" } );
		else if( !ids.insert( tile.Id ).second ) issues.push_back( { tile.Id, "duplicate-id" } );
		if( tile.Type == "unavailable" ) issues.push_back( { tile.Id, "unresolved-placeholder" } );
		if( tile.X < 0 || tile.Y < 0 || tile.W < 1 || tile.H < 1 || tile.X + tile.W > doc.Cols || tile.Y + tile.H > doc.Rows )
			issues.push_back( { tile.Id, "out-of-bounds" } );
		for( size_t j = 0; j < i; ++j )
			if( Overlaps( tile, doc.Tiles[j] ) ) { issues.push_back( { tile.Id, "overlap" } ); break; }
		if( tile.Type == "camera" && tile.Camera > 0 && !placedCameras.insert( tile.Camera ).second )
			issues.push_back( { tile.Id, "duplicate-camera" } );

		std::set<int> refs;
		if( tile.Camera > 0 ) refs.insert( tile.Camera );
		if( tile.IdleCamera > 0 ) refs.insert( tile.IdleCamera );
		refs.insert( tile.Sources.begin(), tile.Sources.end() );
		refs.insert( tile.ScopeCameras.begin(), tile.ScopeCameras.end() );
		for( int id : refs )
			if( !allowedCameras.count( id ) ) { issues.push_back( { tile.Id, "camera-not-available" } ); break; }
	}

	std::set<std::string> badgeIds;
	for( const Badge& badge : doc.Badges )
	{
		if( !std::regex_match( badge.Id, IdPattern ) || !badgeIds.insert( badge.Id ).second )
			issues.push_back( { badge.Id, "invalid-badge-id" } );
	}

	for( const auto* list : { &doc.Priority, &doc.MobileOrder } )
		for( int id : *list )
			if( !allowedCameras.count( id ) ) { issues.push_back( { "", "camera-not-available" } ); break; }

	return issues;
}

void MergeRedacted( Document& incoming, const Document& stored, const std::set<int>& editorAccess )
{
	std::map<std::string, const Tile*> storedById;
	for( const Tile& tile : stored.Tiles ) storedById[tile.Id] = &tile;

	std::vector<Tile> merged;
	for( Tile& tile : incoming.Tiles )
	{
		auto previous = storedById.find( tile.Id );
		if( tile.Type == "unavailable" )
		{
			// Unknown placeholders cannot be restored; drop them.
			if( previous == storedById.end() || previous->second->Type == "unavailable" ) continue;
			Tile restored = *previous->second;
			restored.X = tile.X; restored.Y = tile.Y; restored.W = tile.W; restored.H = tile.H;
			merged.push_back( std::move( restored ) );
			continue;
		}
		if( previous != storedById.end() && previous->second->Type == tile.Type )
		{
			const Tile& old = *previous->second;
			AppendMissing( tile.Sources, Hidden( old.Sources, editorAccess ), {} );
			if( tile.ScopeKind == "list" && old.ScopeKind == "list" )
				AppendMissing( tile.ScopeCameras, Hidden( old.ScopeCameras, editorAccess ), {} );
			if( tile.Type == "spotlight" && tile.IdleKind == "blank" && old.IdleKind == "camera"
				&& old.IdleCamera > 0 && !editorAccess.count( old.IdleCamera ) )
			{
				tile.IdleKind = "camera";
				tile.IdleCamera = old.IdleCamera;
			}
		}
		merged.push_back( std::move( tile ) );
	}
	incoming.Tiles = std::move( merged );

	AppendMissing( incoming.Priority, Hidden( stored.Priority, editorAccess ), {} );
	AppendMissing( incoming.MobileOrder, Hidden( stored.MobileOrder, editorAccess ), {} );
}

int FilterForViewer( Document& doc, const std::set<int>& accessible )
{
	int redacted = 0;
	for( Tile& tile : doc.Tiles )
	{
		if( ( tile.Type == "camera" || tile.Type == "ptz" ) && !accessible.count( tile.Camera ) )
		{
			MakeUnavailable( tile );
			++redacted;
			continue;
		}
		if( tile.Type == "spotlight" )
		{
			Retain( tile.Sources, accessible, redacted );
			if( tile.IdleCamera > 0 && !accessible.count( tile.IdleCamera ) )
			{
				tile.IdleKind = "blank";
				tile.IdleCamera = 0;
				++redacted;
			}
			if( tile.SourcesKind == "list" && tile.Sources.empty() )
			{
				MakeUnavailable( tile );
				continue;
			}
		}
		if( UsesScope( tile.Type ) )
			Retain( tile.ScopeCameras, accessible, redacted );
	}
	Retain( doc.Priority, accessible, redacted );
	Retain( doc.MobileOrder, accessible, redacted );
	return redacted;
}

std::set<int> ReferencedCameras( const Document& doc )
{
	std::set<int> refs;
	for( const Tile& tile : doc.Tiles )
	{
		if( tile.Camera > 0 ) refs.insert( tile.Camera );
		if( tile.IdleCamera > 0 ) refs.insert( tile.IdleCamera );
		refs.insert( tile.Sources.begin(), tile.Sources.end() );
		refs.insert( tile.ScopeCameras.begin(), tile.ScopeCameras.end() );
	}
	refs.insert( doc.Priority.begin(), doc.Priority.end() );
	refs.insert( doc.MobileOrder.begin(), doc.MobileOrder.end() );
	return refs;
}

std::vector<int> ReadingOrderCameras( const Document& doc )
{
	std::vector<const Tile*> ordered;
	for( const Tile& tile : doc.Tiles ) ordered.push_back( &tile );
	std::stable_sort( ordered.begin(), ordered.end(), []( const Tile* a, const Tile* b )
		{ return a->Y != b->Y ? a->Y < b->Y : a->X < b->X; } );

	std::vector<int> result = doc.MobileOrder;
	auto add = [&]( int id ) { if( id > 0 && std::find( result.begin(), result.end(), id ) == result.end() ) result.push_back( id ); };
	for( const Tile* tile : ordered )
	{
		if( tile->Type == "camera" ) add( tile->Camera );
		else if( tile->Type == "spotlight" ) for( int id : tile->Sources ) add( id );
	}
	return result;
}

std::string Emit( const Document& doc )
{
	wvalue root;
	root["schema"] = SchemaVersion;
	root["kind"] = "canvas";
	root["grid"]["cols"] = doc.Cols;
	root["grid"]["rows"] = doc.Rows;
	root["grid"]["gap"] = doc.Gap;
	root["grid"]["radius"] = doc.Radius;
	root["target"]["width"] = doc.TargetWidth;
	root["target"]["height"] = doc.TargetHeight;

	std::vector<wvalue> tiles;
	for( const Tile& tile : doc.Tiles )
	{
		wvalue t;
		t["id"] = tile.Id;
		t["type"] = tile.Type;
		t["x"] = tile.X; t["y"] = tile.Y; t["w"] = tile.W; t["h"] = tile.H;
		if( tile.Type == "camera" || tile.Type == "ptz" )
		{
			t["camera"] = tile.Camera;
			t["label"] = tile.Label;
			if( tile.Type == "camera" )
			{
				t["stream"] = tile.Stream;
				t["fit"] = tile.Fit;
				t["playback"] = tile.Playback;
			}
		}
		else if( tile.Type == "spotlight" )
		{
			t["trigger"] = tile.Trigger;
			t["sources"]["kind"] = tile.SourcesKind;
			t["sources"]["cameras"] = IntList( tile.Sources );
			t["borrowPlaced"] = tile.BorrowPlaced;
			t["holdSeconds"] = tile.HoldSeconds;
			t["idle"]["kind"] = tile.IdleKind;
			if( tile.IdleKind == "camera" ) t["idle"]["camera"] = tile.IdleCamera;
			if( tile.IdleKind == "rotate" ) t["idle"]["everySeconds"] = tile.IdleEverySeconds;
			t["warm"] = tile.Warm;
			t["minSplit"] = tile.MinSplit;
			t["maxSplit"] = tile.MaxSplit;
			t["label"] = tile.Label;
		}
		else if( UsesScope( tile.Type ) )
		{
			t["scope"]["kind"] = tile.ScopeKind;
			if( tile.ScopeKind == "list" ) t["scope"]["cameras"] = IntList( tile.ScopeCameras );
			if( tile.Type == "activity" )
			{
				t["direction"] = tile.Direction;
				t["grouping"] = tile.Grouping;
				t["objectFocus"] = tile.ObjectFocus;
			}
			if( tile.Type == "timeline" ) t["hours"] = tile.Hours;
		}
		else if( tile.Type == "clock" )
		{
			t["seconds"] = tile.Seconds;
			t["date"] = tile.Date;
		}
		else if( tile.Type == "status" )
		{
			t["variant"] = tile.Variant;
		}
		else if( tile.Type == "text" )
		{
			t["text"] = tile.Text;
			t["size"] = tile.Size;
		}
		else if( tile.Type == "unavailable" )
		{
			t["reason"] = tile.Reason;
			t["redacted"] = true;
		}
		tiles.push_back( std::move( t ) );
	}
	root["tiles"] = wvalue( std::move( tiles ) );

	std::vector<wvalue> badges;
	for( const Badge& badge : doc.Badges )
	{
		wvalue b;
		b["id"] = badge.Id;
		b["type"] = badge.Type;
		b["anchor"] = badge.Anchor;
		b["size"] = badge.Size;
		badges.push_back( std::move( b ) );
	}
	root["badges"] = wvalue( std::move( badges ) );

	root["behavior"]["priority"] = IntList( doc.Priority );
	root["behavior"]["returnToLiveSeconds"] = doc.ReturnToLiveSeconds;
	root["behavior"]["quietControls"] = doc.QuietControls;
	root["behavior"]["mainStreams"] = doc.MainStreams;
	root["mobileOrder"] = IntList( doc.MobileOrder );
	return root.dump();
}

std::string IssuesJson( const std::vector<Issue>& issues )
{
	std::vector<wvalue> list;
	for( const Issue& issue : issues )
	{
		wvalue item;
		item["tile"] = issue.Tile;
		item["code"] = issue.Code;
		list.push_back( std::move( item ) );
	}
	wvalue root;
	root["error"] = issues.empty() ? "invalid" : issues.front().Code;
	root["issues"] = wvalue( std::move( list ) );
	return root.dump();
}
}
