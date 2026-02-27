#include "SQLite.h"
#include "Common.h"

#define AssertQuery( condition, message, ... ) if( !(condition) ) { m_database->ThrowError( StringPrintfA( message, __VA_ARGS__ ) ); }
#define AssertDB( condition, message, ... ) if( !(condition) ) { ThrowError( StringPrintfA( message, __VA_ARGS__ ) ); }

SQLiteDatabaseQuery::SQLiteDatabaseQuery(std::shared_ptr<SQLiteDatabase> database )
: m_database( database )	
, m_lastInsertId( -1 )
, m_reset( true )
{
}

SQLiteDatabaseQuery::~SQLiteDatabaseQuery()
{
	for( auto& statement : m_statements )
	{
		sqlite3_finalize( statement );
	}
}

void SQLiteDatabaseQuery::Bind( const char* paramName, const char* value )
{
	Reset();

	for( auto& statement : m_statements )
	{
		int index = sqlite3_bind_parameter_index( statement, paramName );
		if( index )
		{
			int result = sqlite3_bind_text( statement, index, value, -1, SQLITE_TRANSIENT );
			AssertQuery( result == 0, "Failed to bind parameter: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
		}
	}
}

void SQLiteDatabaseQuery::Bind( const char* paramName, double value )
{
	Reset();

	for( auto& statement : m_statements )
	{
		int index = sqlite3_bind_parameter_index( statement, paramName );
		if( index )
		{
			int result = sqlite3_bind_double( statement, index, value );
			AssertQuery( result == 0, "Failed to bind parameter: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
		}
	}
}

void SQLiteDatabaseQuery::Bind( const char* paramName, int value )
{
	Reset();

	for( auto& statement : m_statements )
	{
		int index = sqlite3_bind_parameter_index( statement, paramName );
		if( index )
		{
			int result = sqlite3_bind_int( statement, index, value );
			AssertQuery( result == 0, "Failed to bind parameter: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
		}
	}
}

void SQLiteDatabaseQuery::Bind( const char* paramName, int64_t value )
{
	Reset();

	for( auto& statement : m_statements )
	{
		int index = sqlite3_bind_parameter_index( statement, paramName );
		if( index )
		{
			int result = sqlite3_bind_int64( statement, index, value );
			AssertQuery( result == 0, "Failed to bind parameter: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
		}
	}
}

void SQLiteDatabaseQuery::Reset()
{
	if( !m_reset )
	{
		for( auto& statement : m_statements )
		{
			sqlite3_reset( statement );
		}
		m_reset = true;
	}
}

int SQLiteDatabaseQuery::Execute( const std::function< bool(const SQLiteDatabaseQuery&) >& callback )
{
	m_reset = false;

	int count = 0;

	for( auto& statement : m_statements )
	{
		int result;
		while( (result = sqlite3_step( statement )) == SQLITE_ROW )
		{
			count++;

			if( callback && !callback(*this) )
			{
				break;
			}
		}

		AssertQuery( result == SQLITE_DONE, "Error while reading rows: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
		if (result != SQLITE_DONE)
		{
			std::string ErrorString(sqlite3_errmsg( m_database->GetDatabase() ));


			m_lastError = StringT( ErrorString.begin(), ErrorString.end() );
			return -1;
		}
	}

	m_lastInsertId = sqlite3_last_insert_rowid( m_database->GetDatabase() );

	Reset();

	return count;
}

const char* SQLiteDatabaseQuery::GetColumnValueText( int column ) const
{
	return (const char*)sqlite3_column_text( m_statements.back(), column );
}

sqlite3_value* SQLiteDatabaseQuery::GetColumnValue( int column ) const
{
	return sqlite3_column_value( m_statements.back(), column );
}

const int SQLiteDatabaseQuery::GetColumnValueInt( int column ) const
{
	return sqlite3_column_int( m_statements.back(), column );
}

const int64_t SQLiteDatabaseQuery::GetColumnValueInt64( int column ) const
{
	return sqlite3_column_int64( m_statements.back(), column );
}

const double SQLiteDatabaseQuery::GetColumnValueDouble( int column ) const
{
	return sqlite3_column_double( m_statements.back(), column );
}

const int SQLiteDatabaseQuery::GetColumnCount() const
{
	return sqlite3_column_count( m_statements.back() );
}

SQLiteDatabase::SQLiteDatabase( const StringT& filename, const std::string& initScript, bool writeAccess, std::function<void(const std::string & )> onErrorCallback )
: m_onErrorCallback( onErrorCallback )
, m_filename( filename )
, m_database( nullptr )
, m_databaseNewlyCreated( false )
{
	if (!m_onErrorCallback)
	{
		m_onErrorCallback = [](std::string error){};
	}

	int flags = 0;
	if( writeAccess )
	{
		flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_SHAREDCACHE | SQLITE_OPEN_WAL | SQLITE_OPEN_EXCLUSIVE;
	}
	else
	{
		flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_SHAREDCACHE | SQLITE_OPEN_WAL;
	}

	std::string NewFilename = StringToAnsi(filename);

	int result = sqlite3_open_v2( NewFilename.c_str(), &m_database, flags, nullptr );
	AssertDB( result == 0, "Failed to open database: %s\n%s", NewFilename.c_str(), sqlite3_errmsg( m_database ) );

	if( initScript.length() > 0 )
	{
		char* errorMessage = nullptr;
		result = sqlite3_exec( m_database, initScript.c_str(), NULL, NULL, &errorMessage );
		AssertDB( result == 0, "Failed to execute init script: %s\n%s", NewFilename.c_str(), sqlite3_errmsg( m_database ) );

		if( errorMessage )
		{
			sqlite3_free( errorMessage );
		}
	}
}

SQLiteDatabase::~SQLiteDatabase()
{
	if( m_database )
	{
		sqlite3_close_v2( m_database );
		m_database = nullptr;
	}
}

void SQLiteDatabase::ThrowError( const std::string& Message )
{
	m_onErrorCallback( Message );
}

std::shared_ptr<SQLiteDatabaseQuery> SQLiteDatabase::CreateQuery( const StringT& queryName, const StringT& query )
{
	AssertDB( m_database, "Database was not valid" );

	auto generatedQuery = std::make_shared<SQLiteDatabaseQuery>( shared_from_this() );

	StringT newQuery = query;
	const char* nextStatement = newQuery.c_str();
	do{
		sqlite3_stmt* newStatement = nullptr;
		newQuery = Trim(nextStatement);
		if( newQuery.length() )
		{
			int result = sqlite3_prepare_v2( m_database, newQuery.c_str(), -1, &newStatement, &nextStatement );
			AssertDB( result == 0 && newStatement, "Failed to prepare statement: %s\n:%s", newQuery.c_str(), sqlite3_errmsg( m_database ) );

			generatedQuery->AddStatement( newStatement );
		}
	} while( *nextStatement != '\0' );

	m_queries[ queryName ] = generatedQuery;

	return generatedQuery;
}
