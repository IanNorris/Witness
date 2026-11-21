#include "DashboardStorage.h"

#include <sqlite3.h>

#include <stdexcept>

namespace Dashboard
{
namespace
{
	void Check( bool ok, const char* message )
	{
		if( !ok ) throw std::runtime_error( message );
	}

	struct Statement
	{
		sqlite3_stmt* Value = nullptr;
		Statement( sqlite3* db, const char* sql )
		{
			Check( sqlite3_prepare_v2( db, sql, -1, &Value, nullptr ) == SQLITE_OK, "Dashboard query preparation failed" );
		}
		~Statement() { sqlite3_finalize( Value ); }
		Statement( const Statement& ) = delete;
		Statement& operator=( const Statement& ) = delete;

		Statement& Int( int index, int64_t v ) { Check( sqlite3_bind_int64( Value, index, v ) == SQLITE_OK, "Dashboard bind failed" ); return *this; }
		Statement& Text( int index, const std::string& v ) { Check( sqlite3_bind_text( Value, index, v.c_str(), static_cast<int>( v.size() ), SQLITE_TRANSIENT ) == SQLITE_OK, "Dashboard bind failed" ); return *this; }
		bool Step()
		{
			const int rc = sqlite3_step( Value );
			Check( rc == SQLITE_ROW || rc == SQLITE_DONE, "Dashboard query failed" );
			return rc == SQLITE_ROW;
		}
		int64_t ColInt( int column ) const { return sqlite3_column_int64( Value, column ); }
		std::string ColText( int column ) const
		{
			const auto* text = reinterpret_cast<const char*>( sqlite3_column_text( Value, column ) );
			return text ? std::string( text, static_cast<size_t>( sqlite3_column_bytes( Value, column ) ) ) : std::string();
		}
	};

	const char* RowColumns = "DashboardUID, OwnerUserUID, Scope, Name, Document, Revision, CreatedAt, UpdatedAt, UpdatedByUserUID";

	Row ReadRow( const Statement& s )
	{
		Row row;
		row.Id = s.ColInt( 0 );
		row.OwnerUserUID = static_cast<int>( s.ColInt( 1 ) );
		row.DashboardScope = s.ColInt( 2 ) == 1 ? Scope::Shared : Scope::Personal;
		row.Name = s.ColText( 3 );
		row.Document = s.ColText( 4 );
		row.Revision = static_cast<int>( s.ColInt( 5 ) );
		row.CreatedAt = s.ColInt( 6 );
		row.UpdatedAt = s.ColInt( 7 );
		row.UpdatedByUserUID = static_cast<int>( s.ColInt( 8 ) );
		return row;
	}

	void SnapshotRevision( sqlite3* db, int64_t id, int revision, const std::string& name, const std::string& document, int userUID, int64_t now )
	{
		Statement insert( db, "INSERT OR REPLACE INTO DashboardRevision(DashboardUID, Revision, Name, Document, UpdatedAt, UpdatedByUserUID) VALUES(?,?,?,?,?,?);" );
		insert.Int( 1, id ).Int( 2, revision ).Text( 3, name ).Text( 4, document ).Int( 5, now ).Int( 6, userUID ).Step();
		Statement prune( db, "DELETE FROM DashboardRevision WHERE DashboardUID = ? AND Revision <= ?;" );
		prune.Int( 1, id ).Int( 2, revision - KeptRevisions ).Step();
	}
}

void EnsureSchema( sqlite3* db )
{
	Check( sqlite3_exec( db, R"SQL(
		CREATE TABLE IF NOT EXISTS Dashboard(
			DashboardUID INTEGER PRIMARY KEY AUTOINCREMENT,
			OwnerUserUID INTEGER NOT NULL,
			Scope INTEGER NOT NULL DEFAULT 0,
			Kind INTEGER NOT NULL DEFAULT 0,
			Name TEXT NOT NULL,
			Document TEXT NOT NULL,
			SchemaVersion INTEGER NOT NULL DEFAULT 2,
			Revision INTEGER NOT NULL DEFAULT 1,
			CreatedAt INTEGER NOT NULL,
			UpdatedAt INTEGER NOT NULL,
			UpdatedByUserUID INTEGER NOT NULL);
		CREATE INDEX IF NOT EXISTS DashboardByOwner ON Dashboard(OwnerUserUID, Scope);
		CREATE TABLE IF NOT EXISTS DashboardShare(
			DashboardUID INTEGER NOT NULL,
			PrincipalType INTEGER NOT NULL,
			PrincipalUID INTEGER NOT NULL,
			PRIMARY KEY(DashboardUID, PrincipalType, PrincipalUID),
			FOREIGN KEY(DashboardUID) REFERENCES Dashboard(DashboardUID) ON DELETE CASCADE) WITHOUT ROWID;
		CREATE INDEX IF NOT EXISTS DashboardShareByPrincipal ON DashboardShare(PrincipalType, PrincipalUID);
		CREATE TABLE IF NOT EXISTS DashboardRevision(
			DashboardUID INTEGER NOT NULL,
			Revision INTEGER NOT NULL,
			Name TEXT NOT NULL,
			Document TEXT NOT NULL,
			UpdatedAt INTEGER NOT NULL,
			UpdatedByUserUID INTEGER NOT NULL,
			PRIMARY KEY(DashboardUID, Revision),
			FOREIGN KEY(DashboardUID) REFERENCES Dashboard(DashboardUID) ON DELETE CASCADE) WITHOUT ROWID;
	)SQL", nullptr, nullptr, nullptr ) == SQLITE_OK, "Could not initialize dashboard schema" );
}

bool CanEdit( const Row& row, int userUID, bool admin )
{
	return row.DashboardScope == Scope::Personal ? row.OwnerUserUID == userUID : admin;
}

bool CanView( sqlite3* db, const Row& row, int userUID, bool admin )
{
	if( row.DashboardScope == Scope::Personal )
		return row.OwnerUserUID == userUID;
	if( admin )
		return true;
	Statement s( db, R"SQL(
		SELECT 1 FROM DashboardShare S
		WHERE S.DashboardUID = ?1 AND (
			S.PrincipalType = 0
			OR (S.PrincipalType = 1 AND S.PrincipalUID = ?2)
			OR (S.PrincipalType = 2 AND S.PrincipalUID IN (SELECT `Group` FROM UserGroupMapping WHERE UserUID = ?2)))
		LIMIT 1;)SQL" );
	s.Int( 1, row.Id ).Int( 2, userUID );
	return s.Step();
}

std::vector<Row> ListVisible( sqlite3* db, int userUID, bool admin )
{
	const std::string sql = std::string( "SELECT " ) + RowColumns + R"SQL( FROM Dashboard D
		WHERE (D.Scope = 0 AND D.OwnerUserUID = ?1)
			OR (D.Scope = 1 AND (?2 = 1 OR EXISTS (
				SELECT 1 FROM DashboardShare S WHERE S.DashboardUID = D.DashboardUID AND (
					S.PrincipalType = 0
					OR (S.PrincipalType = 1 AND S.PrincipalUID = ?1)
					OR (S.PrincipalType = 2 AND S.PrincipalUID IN (SELECT `Group` FROM UserGroupMapping WHERE UserUID = ?1))))))
		ORDER BY D.Scope, D.Name COLLATE NOCASE, D.DashboardUID;)SQL";
	Statement s( db, sql.c_str() );
	s.Int( 1, userUID ).Int( 2, admin ? 1 : 0 );
	std::vector<Row> rows;
	while( s.Step() ) rows.push_back( ReadRow( s ) );
	return rows;
}

std::optional<Row> Get( sqlite3* db, int64_t id )
{
	const std::string sql = std::string( "SELECT " ) + RowColumns + " FROM Dashboard WHERE DashboardUID = ?;";
	Statement s( db, sql.c_str() );
	s.Int( 1, id );
	if( !s.Step() ) return std::nullopt;
	return ReadRow( s );
}

int CountOwnedPersonal( sqlite3* db, int userUID )
{
	Statement s( db, "SELECT COUNT(*) FROM Dashboard WHERE OwnerUserUID = ? AND Scope = 0;" );
	s.Int( 1, userUID );
	s.Step();
	return static_cast<int>( s.ColInt( 0 ) );
}

int64_t Create( sqlite3* db, int ownerUserUID, Scope scope, const std::string& name, const std::string& document, int64_t now )
{
	Statement s( db, R"SQL(INSERT INTO Dashboard(OwnerUserUID, Scope, Kind, Name, Document, SchemaVersion, Revision, CreatedAt, UpdatedAt, UpdatedByUserUID)
		VALUES(?1, ?2, 0, ?3, ?4, 2, 1, ?5, ?5, ?1) RETURNING DashboardUID;)SQL" );
	s.Int( 1, ownerUserUID ).Int( 2, static_cast<int>( scope ) ).Text( 3, name ).Text( 4, document ).Int( 5, now );
	Check( s.Step(), "Dashboard insert returned no id" );
	const int64_t id = s.ColInt( 0 );
	s.Step();
	SnapshotRevision( db, id, 1, name, document, ownerUserUID, now );
	return id;
}

UpdateResult Update( sqlite3* db, int64_t id, int baseRevision, const std::string& name, const std::string& document,
	int userUID, int64_t now, int* newRevision )
{
	int revision = 0;
	{
		// Optimistic concurrency: only the holder of the current revision wins.
		Statement s( db, R"SQL(UPDATE Dashboard SET Name = ?1, Document = ?2, Revision = Revision + 1, UpdatedAt = ?3, UpdatedByUserUID = ?4
			WHERE DashboardUID = ?5 AND Revision = ?6 RETURNING Revision;)SQL" );
		s.Text( 1, name ).Text( 2, document ).Int( 3, now ).Int( 4, userUID ).Int( 5, id ).Int( 6, baseRevision );
		if( s.Step() )
		{
			revision = static_cast<int>( s.ColInt( 0 ) );
			s.Step();
		}
	}
	if( revision == 0 )
		return Get( db, id ) ? UpdateResult::Conflict : UpdateResult::NotFound;
	SnapshotRevision( db, id, revision, name, document, userUID, now );
	if( newRevision ) *newRevision = revision;
	return UpdateResult::Ok;
}

bool Delete( sqlite3* db, int64_t id )
{
	Statement s( db, "DELETE FROM Dashboard WHERE DashboardUID = ? RETURNING DashboardUID;" );
	s.Int( 1, id );
	const bool deleted = s.Step();
	if( deleted ) s.Step();
	return deleted;
}

std::vector<Principal> GetShares( sqlite3* db, int64_t id )
{
	Statement s( db, "SELECT PrincipalType, PrincipalUID FROM DashboardShare WHERE DashboardUID = ? ORDER BY PrincipalType, PrincipalUID;" );
	s.Int( 1, id );
	std::vector<Principal> result;
	while( s.Step() )
		result.push_back( { static_cast<PrincipalType>( s.ColInt( 0 ) ), static_cast<int>( s.ColInt( 1 ) ) } );
	return result;
}

void SetShares( sqlite3* db, int64_t id, const std::vector<Principal>& principals )
{
	{
		Statement clear( db, "DELETE FROM DashboardShare WHERE DashboardUID = ?;" );
		clear.Int( 1, id ).Step();
	}
	for( const Principal& principal : principals )
	{
		Statement insert( db, "INSERT OR IGNORE INTO DashboardShare(DashboardUID, PrincipalType, PrincipalUID) VALUES(?,?,?);" );
		insert.Int( 1, id ).Int( 2, static_cast<int>( principal.Type ) )
			.Int( 3, principal.Type == PrincipalType::Everyone ? 0 : principal.Id ).Step();
	}
}

std::vector<RevisionInfo> ListRevisions( sqlite3* db, int64_t id )
{
	Statement s( db, "SELECT Revision, UpdatedAt, UpdatedByUserUID, Name FROM DashboardRevision WHERE DashboardUID = ? ORDER BY Revision DESC;" );
	s.Int( 1, id );
	std::vector<RevisionInfo> result;
	while( s.Step() )
		result.push_back( { static_cast<int>( s.ColInt( 0 ) ), s.ColInt( 1 ), static_cast<int>( s.ColInt( 2 ) ), s.ColText( 3 ) } );
	return result;
}

std::optional<std::string> GetRevisionDocument( sqlite3* db, int64_t id, int revision )
{
	Statement s( db, "SELECT Document FROM DashboardRevision WHERE DashboardUID = ? AND Revision = ?;" );
	s.Int( 1, id ).Int( 2, revision );
	if( !s.Step() ) return std::nullopt;
	return s.ColText( 0 );
}
}
