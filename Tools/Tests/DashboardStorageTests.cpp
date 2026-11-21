// Dashboard storage against an in-memory database: the visibility matrix
// (personal / everyone / user / group / admin), optimistic revisions,
// revision history pruning and cascades.
#include "DashboardStorage.h"

#include <sqlite3.h>

#include <iostream>
#include <memory>
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

	int Count( sqlite3* db, const char* sql )
	{
		sqlite3_stmt* s = nullptr;
		if( sqlite3_prepare_v2( db, sql, -1, &s, nullptr ) != SQLITE_OK ) throw std::runtime_error( "count prepare" );
		sqlite3_step( s );
		const int value = sqlite3_column_int( s, 0 );
		sqlite3_finalize( s );
		return value;
	}

	bool Visible( sqlite3* db, int user, bool admin, int64_t id )
	{
		for( const auto& row : ListVisible( db, user, admin ) ) if( row.Id == id ) return true;
		return false;
	}
}

int main()
{
	sqlite3* raw = nullptr;
	if( sqlite3_open( ":memory:", &raw ) != SQLITE_OK ) return 2;
	std::unique_ptr<sqlite3, decltype( &sqlite3_close )> holder( raw, sqlite3_close );
	try
	{
		sqlite3_exec( raw, "PRAGMA foreign_keys=ON;"
			"CREATE TABLE UserGroupMapping(UserUID INTEGER NOT NULL, `Group` INTEGER NOT NULL);"
			"INSERT INTO UserGroupMapping VALUES(2, 10);", nullptr, nullptr, nullptr );
		EnsureSchema( raw );
		EnsureSchema( raw ); // idempotent

		// Users: 1 = admin, 2 = member of group 10, 3 = no groups.
		const int64_t personal = Create( raw, 2, Scope::Personal, "Mine", "{}", 100 );
		const int64_t shared = Create( raw, 1, Scope::Shared, "Wall", "{}", 100 );
		const int64_t adminPersonal = Create( raw, 1, Scope::Personal, "Admin private", "{}", 100 );

		Expect( Visible( raw, 2, false, personal ), "owner sees personal" );
		Expect( !Visible( raw, 3, false, personal ), "other user cannot see personal" );
		Expect( !Visible( raw, 1, true, personal ), "admin cannot see someone else's personal" );
		Expect( !Visible( raw, 2, false, adminPersonal ), "user cannot see admin personal" );

		Expect( Visible( raw, 1, true, shared ), "admin sees all shared" );
		Expect( !Visible( raw, 2, false, shared ), "unshared not visible" );

		SetShares( raw, shared, { { PrincipalType::Group, 10 } } );
		Expect( Visible( raw, 2, false, shared ), "group member sees shared" );
		Expect( !Visible( raw, 3, false, shared ), "non-member does not" );
		auto row = Get( raw, shared );
		Expect( row && CanView( raw, *row, 2, false ) && !CanView( raw, *row, 3, false ), "CanView matches list" );

		SetShares( raw, shared, { { PrincipalType::User, 3 } } );
		Expect( !Visible( raw, 2, false, shared ) && Visible( raw, 3, false, shared ), "user principal replaces group" );
		SetShares( raw, shared, { { PrincipalType::Everyone, 0 } } );
		Expect( Visible( raw, 2, false, shared ) && Visible( raw, 3, false, shared ), "everyone principal" );
		Expect( GetShares( raw, shared ).size() == 1, "shares listed" );

		// Edit rights.
		Expect( CanEdit( *Get( raw, personal ), 2, false ), "owner edits personal" );
		Expect( !CanEdit( *Get( raw, personal ), 1, true ), "admin cannot edit another's personal" );
		Expect( CanEdit( *row, 1, true ), "admin edits shared" );
		Expect( !CanEdit( *row, 2, false ), "recipient cannot edit shared" );

		// Optimistic revisions.
		int revision = 0;
		Expect( Update( raw, personal, 1, "Mine", "{\"a\":1}", 2, 200, &revision ) == UpdateResult::Ok && revision == 2, "update bumps revision" );
		Expect( Update( raw, personal, 1, "Mine", "{\"a\":2}", 2, 201, &revision ) == UpdateResult::Conflict, "stale revision conflicts" );
		Expect( Get( raw, personal )->Document == "{\"a\":1}", "conflict did not write" );
		Expect( Update( raw, 9999, 1, "x", "{}", 2, 201, &revision ) == UpdateResult::NotFound, "missing dashboard" );

		// History keeps the last KeptRevisions entries.
		for( int base = 2; base < 2 + KeptRevisions + 3; ++base )
			Update( raw, personal, base, "Mine", "{\"rev\":" + std::to_string( base + 1 ) + "}", 2, 300 + base, &revision );
		auto history = ListRevisions( raw, personal );
		Expect( static_cast<int>( history.size() ) == KeptRevisions, "history pruned" );
		Expect( history.front().Revision == revision, "newest revision first" );
		Expect( GetRevisionDocument( raw, personal, revision ).value_or( "" ) == "{\"rev\":" + std::to_string( revision ) + "}", "revision document" );
		Expect( !GetRevisionDocument( raw, personal, 1 ), "old revision pruned" );

		Expect( CountOwnedPersonal( raw, 1 ) == 1 && CountOwnedPersonal( raw, 2 ) == 1, "personal counts" );

		// Cascades.
		Expect( Delete( raw, shared ), "delete" );
		Expect( !Delete( raw, shared ), "second delete reports missing" );
		Expect( Count( raw, "SELECT COUNT(*) FROM DashboardShare;" ) == 0, "shares cascade" );
		Delete( raw, personal );
		Expect( Count( raw, "SELECT COUNT(*) FROM DashboardRevision WHERE DashboardUID = 1;" ) == 0, "revisions cascade" );
	}
	catch( const std::exception& e )
	{
		std::cerr << "Exception: " << e.what() << "\n";
		return 2;
	}
	if( Failures ) { std::cerr << Failures << " failure(s)\n"; return 1; }
	std::cout << "DashboardStorageTests passed\n";
	return 0;
}
