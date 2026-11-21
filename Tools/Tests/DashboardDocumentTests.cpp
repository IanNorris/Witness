// Pure tests for the dashboards v2 document model: validation, canonical
// emission, per-viewer filtering and redaction round-trips.
#include "DashboardDocument.h"

#include <crow/json.h>

#include <iostream>
#include <stdexcept>
#include <string>

using namespace Dashboard;

namespace
{
	int Failures = 0;
	void Expect( bool ok, const std::string& message )
	{
		if( !ok ) { std::cerr << "FAIL: " << message << "\n"; ++Failures; }
	}

	bool HasCode( const std::vector<Issue>& issues, const std::string& code )
	{
		for( const auto& issue : issues ) if( issue.Code == code ) return true;
		return false;
	}

	Document ParseOk( const std::string& json, bool allowUnavailable = false )
	{
		auto result = Parse( json, allowUnavailable );
		if( !result.Ok )
		{
			std::cerr << "Unexpected parse failure: " << ( result.Issues.empty() ? "?" : result.Issues.front().Code ) << "\n";
			throw std::runtime_error( "parse" );
		}
		return result.Doc;
	}

	const char* Sample = R"({
		"schema": 2, "grid": {"cols": 48, "rows": 48, "gap": 4, "radius": 8},
		"unknownTopLevel": "dropped",
		"tiles": [
			{"id": "a", "type": "camera", "x": 0, "y": 0, "w": 24, "h": 24, "camera": 1, "secret": 42},
			{"id": "b", "type": "camera", "x": 24, "y": 0, "w": 24, "h": 24, "camera": 2},
			{"id": "s", "type": "spotlight", "x": 0, "y": 24, "w": 24, "h": 24,
			 "sources": {"kind": "list", "cameras": [3, 2, 1]}, "idle": {"kind": "camera", "camera": 3},
			 "maxSplit": 4, "holdSeconds": 20},
			{"id": "act", "type": "activity", "x": 24, "y": 24, "w": 24, "h": 24,
			 "scope": {"kind": "list", "cameras": [1, 3]}}
		],
		"behavior": {"priority": [3, 1, 2]},
		"mobileOrder": [2]
	})";
}

int main()
{
	try
	{
		const std::set<int> all = { 1, 2, 3 };
		const std::set<int> limited = { 1, 2 };

		// Canonicalisation drops unknown fields and round-trips.
		{
			Document doc = ParseOk( Sample );
			Expect( Validate( doc, all ).empty(), "sample validates with full access" );
			const std::string canonical = Emit( doc );
			Expect( canonical.find( "unknownTopLevel" ) == std::string::npos, "unknown top-level field dropped" );
			Expect( canonical.find( "secret" ) == std::string::npos, "unknown tile field dropped" );
			Document again = ParseOk( canonical );
			Expect( Emit( again ) == canonical, "canonical emission is stable" );
			Expect( again.Tiles[2].MaxSplit == 4 && again.Tiles[2].HoldSeconds == 20, "spotlight fields preserved" );
		}

		// Structural validation.
		{
			auto overlap = ParseOk( R"({"tiles":[{"id":"a","type":"clock","x":0,"y":0,"w":10,"h":10},{"id":"b","type":"clock","x":5,"y":5,"w":10,"h":10}]})" );
			Expect( HasCode( Validate( overlap, all ), "overlap" ), "overlap rejected" );
			auto bounds = ParseOk( R"({"grid":{"cols":12,"rows":12},"tiles":[{"id":"a","type":"clock","x":10,"y":0,"w":4,"h":2}]})" );
			Expect( HasCode( Validate( bounds, all ), "out-of-bounds" ), "out of bounds rejected" );
			auto dupId = ParseOk( R"({"tiles":[{"id":"a","type":"clock","x":0,"y":0,"w":4,"h":2},{"id":"a","type":"clock","x":4,"y":0,"w":4,"h":2}]})" );
			Expect( HasCode( Validate( dupId, all ), "duplicate-id" ), "duplicate tile id rejected" );
			auto badId = ParseOk( R"({"tiles":[{"id":"A!","type":"clock","x":0,"y":0,"w":4,"h":2}]})" );
			Expect( HasCode( Validate( badId, all ), "invalid-id" ), "invalid tile id rejected" );
			auto dupCam = ParseOk( R"({"tiles":[{"id":"a","type":"camera","camera":1,"x":0,"y":0,"w":4,"h":2},{"id":"b","type":"camera","camera":1,"x":4,"y":0,"w":4,"h":2}]})" );
			Expect( HasCode( Validate( dupCam, all ), "duplicate-camera" ), "camera placed twice rejected" );

			Expect( !Parse( R"({"tiles":[{"id":"a","type":"iframe","x":0,"y":0,"w":4,"h":2}]})", false ).Ok, "unknown tile type rejected" );
			Expect( !Parse( R"({"tiles":[{"id":"a","type":"unavailable","x":0,"y":0,"w":4,"h":2}]})", false ).Ok, "placeholder rejected on create" );
			Expect( !Parse( R"({"schema":3})", false ).Ok, "unsupported schema rejected" );
			Expect( !Parse( R"({"tiles":[{"id":"a","type":"spotlight","x":0,"y":0,"w":4,"h":2,"maxSplit":9}]})", false ).Ok, "maxSplit bounded" );
			Expect( !Parse( std::string( MaxDocumentBytes + 1, ' ' ), false ).Ok, "oversized document rejected" );
		}

		// Inaccessible and nonexistent cameras are indistinguishable.
		{
			Document doc = ParseOk( Sample );
			auto issues = Validate( doc, limited );
			Expect( HasCode( issues, "camera-not-available" ), "inaccessible camera rejected" );
			auto missing = ParseOk( R"({"tiles":[{"id":"a","type":"camera","camera":999,"x":0,"y":0,"w":4,"h":2}]})" );
			Expect( Validate( missing, limited ).front().Code == issues.front().Code, "missing camera uses the same code" );
		}

		// Viewer filtering removes every trace of camera 3.
		{
			Document doc = ParseOk( Sample );
			const int redacted = FilterForViewer( doc, limited );
			Expect( redacted >= 4, "redactions counted" );
			const std::string filtered = Emit( doc );
			auto root = crow::json::load( filtered );
			Expect( root["tiles"][2]["type"].s() == std::string( "spotlight" ), "spotlight with accessible sources kept" );
			Expect( root["tiles"][2]["sources"]["cameras"].size() == 2, "inaccessible spotlight source removed" );
			Expect( root["tiles"][2]["idle"]["kind"].s() == std::string( "blank" ), "inaccessible idle camera blanked" );
			Expect( root["tiles"][3]["scope"]["cameras"].size() == 1, "inaccessible activity scope removed" );
			Expect( root["behavior"]["priority"].size() == 2, "inaccessible priority removed" );

			Document onlyOne = ParseOk( Sample );
			FilterForViewer( onlyOne, { 1 } );
			const std::string text = Emit( onlyOne );
			auto r = crow::json::load( text );
			Expect( r["tiles"][1]["type"].s() == std::string( "unavailable" ), "inaccessible camera tile becomes placeholder" );
			Expect( !r["tiles"][1].has( "camera" ), "placeholder carries no camera id" );
			Expect( r["tiles"][1]["w"].i() == 24, "placeholder keeps its rectangle" );
		}

		// A spotlight left with no sources becomes unavailable.
		{
			auto doc = ParseOk( R"({"tiles":[{"id":"s","type":"spotlight","x":0,"y":0,"w":8,"h":8,"sources":{"kind":"list","cameras":[3]}}]})" );
			FilterForViewer( doc, limited );
			Expect( doc.Tiles[0].Type == "unavailable", "empty spotlight hidden" );
		}

		// Saving a filtered document restores what the editor never saw.
		{
			Document stored = ParseOk( Sample );
			Document view = stored;
			FilterForViewer( view, { 1 } );
			// The editor moves the placeholder and reorders sources.
			for( auto& tile : view.Tiles ) if( tile.Id == "b" ) { tile.X = 24; tile.Y = 0; tile.W = 20; }
			std::string sent = Emit( view );
			auto incoming = Parse( sent, true );
			Expect( incoming.Ok, "filtered view parses with placeholders" );
			MergeRedacted( incoming.Doc, stored, { 1 } );
			std::set<int> allowed = { 1 };
			for( int ref : ReferencedCameras( stored ) ) allowed.insert( ref );
			Expect( Validate( incoming.Doc, allowed ).empty(), "merged document validates" );
			const Tile* b = nullptr;
			const Tile* s = nullptr;
			for( const auto& tile : incoming.Doc.Tiles ) { if( tile.Id == "b" ) b = &tile; if( tile.Id == "s" ) s = &tile; }
			Expect( b && b->Type == "camera" && b->Camera == 2 && b->W == 20, "placeholder restored with new rectangle" );
			Expect( s && s->Sources.size() == 3, "hidden spotlight sources restored" );
			Expect( s && s->IdleKind == "camera" && s->IdleCamera == 3, "hidden idle camera restored" );
			Expect( incoming.Doc.Priority.size() == 3, "hidden priorities restored" );

			// An unknown placeholder id cannot smuggle in a reference.
			auto forged = Parse( R"({"tiles":[{"id":"zz","type":"unavailable","x":0,"y":0,"w":4,"h":4}]})", true );
			MergeRedacted( forged.Doc, stored, { 1 } );
			Expect( forged.Doc.Tiles.empty(), "unknown placeholder dropped" );
		}

		// Phone reading order: mobileOrder first, then top-to-bottom, left-to-right.
		{
			Document doc = ParseOk( Sample );
			auto order = ReadingOrderCameras( doc );
			Expect( order.size() == 3 && order[0] == 2 && order[1] == 1 && order[2] == 3, "reading order" );
		}
	}
	catch( const std::exception& e )
	{
		std::cerr << "Exception: " << e.what() << "\n";
		return 2;
	}

	if( Failures ) { std::cerr << Failures << " failure(s)\n"; return 1; }
	std::cout << "DashboardDocumentTests passed\n";
	return 0;
}
