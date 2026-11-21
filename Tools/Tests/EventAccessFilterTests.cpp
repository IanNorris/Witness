// Camera-scoped event visibility and the per-user access cache.
#include "EventBroadcaster.h"

#include <iostream>

int main()
{
	int failures = 0;
	auto expect = [&]( bool ok, const char* message ) { if( !ok ) { std::cerr << "FAIL: " << message << "\n"; ++failures; } };

	const std::set<int> allowed = { 1, 4 };
	expect( EventVisibleTo( 0, allowed ), "unscoped events reach everyone" );
	expect( EventVisibleTo( 1, allowed ), "accessible camera visible" );
	expect( !EventVisibleTo( 2, allowed ), "inaccessible camera hidden" );
	expect( !EventVisibleTo( 2, {} ), "no access hides scoped events" );

	EventBroadcaster events;
	int resolves = 0;
	std::set<int> grant = { 1 };
	events.SetAccessResolver( [&]( int user ) { ++resolves; return user == 7 ? grant : std::set<int>{}; } );
	expect( events.AllowedCameras( 7 ) == std::set<int>{ 1 }, "resolver result" );
	events.AllowedCameras( 7 );
	expect( resolves == 1, "access cached per user" );
	grant = { 1, 2 };
	events.InvalidateAccess();
	expect( events.AllowedCameras( 7 ).count( 2 ) == 1, "invalidation picks up new grants" );
	expect( resolves == 2, "resolved again after invalidation" );
	expect( events.AllowedCameras( 8 ).empty(), "unknown user has no access" );

	if( failures ) return 1;
	std::cout << "EventAccessFilterTests passed\n";
	return 0;
}
