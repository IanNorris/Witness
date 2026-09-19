#include "SQLite.h"
#include "Common.h"

#include <Log.h>
#include <atomic>
#include <deque>
#include <thread>

#define AssertQuery( condition, message, ... ) if( !(condition) ) { m_database->ThrowError( "[" + m_queryName + "] " + StringPrintfA( message, __VA_ARGS__ ) ); }
#define AssertDB( condition, message, ... ) if( !(condition) ) { ThrowError( StringPrintfA( message, __VA_ARGS__ ) ); }

namespace
{
	constexpr uint64_t SQLiteContentionThresholdUS = 1000;
	constexpr uint64_t SQLiteTraceWaitThresholdUS = 25000;
	constexpr uint64_t SQLiteTraceScopeThresholdUS = 100000;
	constexpr uint64_t SQLiteWarningThresholdUS = 500000;
	constexpr size_t MaxRecentSQLiteEvents = 64;

	struct SQLiteDiagnosticsState
	{
		std::atomic<uint64_t> Sequence{ 0 };
		std::atomic<uint64_t> Queries{ 0 };
		std::atomic<uint64_t> ContendedQueries{ 0 };
		std::atomic<uint64_t> SlowQueries{ 0 };
		std::atomic<uint64_t> TotalWaitUS{ 0 };
		std::atomic<uint64_t> MaxWaitUS{ 0 };
		std::atomic<uint64_t> TotalScopeUS{ 0 };
		std::atomic<uint64_t> MaxScopeUS{ 0 };
		std::atomic<uint64_t> TotalExecuteUS{ 0 };
		std::atomic<uint64_t> MaxExecuteUS{ 0 };
		std::mutex RecentMutex;
		std::deque<SQLiteQueryTimingEvent> Recent;
	};

	SQLiteDiagnosticsState& GetSQLiteDiagnosticsState()
	{
		static SQLiteDiagnosticsState State;
		return State;
	}

	void UpdateMaximum( std::atomic<uint64_t>& Target, uint64_t Value )
	{
		uint64_t Current = Target.load( std::memory_order_relaxed );
		while( Current < Value && !Target.compare_exchange_weak(
			Current, Value, std::memory_order_relaxed ) ) {}
	}

	void RecordSQLiteQueryTiming( const std::string& Query,
		std::chrono::system_clock::time_point Started, uint64_t WaitUS, uint64_t ScopeUS,
		uint64_t ExecuteUS )
	{
		auto& Diagnostics = GetSQLiteDiagnosticsState();
		++Diagnostics.Queries;
		Diagnostics.TotalWaitUS.fetch_add( WaitUS, std::memory_order_relaxed );
		Diagnostics.TotalScopeUS.fetch_add( ScopeUS, std::memory_order_relaxed );
		Diagnostics.TotalExecuteUS.fetch_add( ExecuteUS, std::memory_order_relaxed );
		UpdateMaximum( Diagnostics.MaxWaitUS, WaitUS );
		UpdateMaximum( Diagnostics.MaxScopeUS, ScopeUS );
		UpdateMaximum( Diagnostics.MaxExecuteUS, ExecuteUS );
		if( WaitUS >= SQLiteContentionThresholdUS ) ++Diagnostics.ContendedQueries;

		const bool Trace = WaitUS >= SQLiteTraceWaitThresholdUS ||
			ScopeUS >= SQLiteTraceScopeThresholdUS;
		if( !Trace ) return;
		++Diagnostics.SlowQueries;
		SQLiteQueryTimingEvent Event;
		Event.Sequence = ++Diagnostics.Sequence;
		Event.StartedUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			Started.time_since_epoch() ).count();
		Event.Query = Query;
		Event.Thread = static_cast<uint64_t>(
			std::hash<std::thread::id>{}( std::this_thread::get_id() ) );
		Event.MutexWaitMs = static_cast<double>( WaitUS ) / 1000.0;
		Event.ScopeMs = static_cast<double>( ScopeUS ) / 1000.0;
		Event.ExecuteMs = static_cast<double>( ExecuteUS ) / 1000.0;
		{
			std::lock_guard Lock( Diagnostics.RecentMutex );
			Diagnostics.Recent.push_back( Event );
			if( Diagnostics.Recent.size() > MaxRecentSQLiteEvents )
				Diagnostics.Recent.pop_front();
		}
		if( WaitUS >= SQLiteWarningThresholdUS || ScopeUS >= SQLiteWarningThresholdUS )
		{
			LOG_WARNING( "[SQLite] Slow query scope %s: mutex wait %.1fms, execute %.1fms, scope %.1fms",
				Query.c_str(), Event.MutexWaitMs, Event.ExecuteMs, Event.ScopeMs );
		}
	}
}

SQLiteDatabaseQueryInstance::SQLiteDatabaseQueryInstance(
	const std::shared_ptr<SQLiteDatabase>& DB, const char* QueryName )
:	m_Query( DB->GetQuery( QueryName ) )
,	m_Lock( m_Query->PrepareQueryMutex(), std::defer_lock )
,	m_WaitStarted( std::chrono::steady_clock::now() )
,	m_StartedSystem( std::chrono::system_clock::now() )
{
	m_Lock.lock();
	m_LockAcquired = std::chrono::steady_clock::now();
	m_Query->ResetInstanceExecuteTime();
	m_MutexWaitUS = static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::microseconds>(
		m_LockAcquired - m_WaitStarted ).count() );
}

SQLiteDatabaseQueryInstance::~SQLiteDatabaseQueryInstance() noexcept
{
	const uint64_t ScopeUS = static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - m_LockAcquired ).count() );
	const std::string& QueryName = m_Query->GetQueryName();
	const uint64_t ExecuteUS = m_Query->GetInstanceExecuteTimeUS();
	if( m_Lock.owns_lock() ) m_Lock.unlock();
	try
	{
		RecordSQLiteQueryTiming( QueryName, m_StartedSystem, m_MutexWaitUS, ScopeUS, ExecuteUS );
	}
	catch( ... )
	{
		// Diagnostics must never turn normal query teardown into process termination.
	}
}

SQLiteDiagnosticsSnapshot GetSQLiteDiagnosticsSnapshot()
{
	auto& Diagnostics = GetSQLiteDiagnosticsState();
	SQLiteDiagnosticsSnapshot Result;
	Result.Queries = Diagnostics.Queries.load( std::memory_order_relaxed );
	Result.ContendedQueries = Diagnostics.ContendedQueries.load( std::memory_order_relaxed );
	Result.SlowQueries = Diagnostics.SlowQueries.load( std::memory_order_relaxed );
	const uint64_t TotalWaitUS = Diagnostics.TotalWaitUS.load( std::memory_order_relaxed );
	const uint64_t TotalScopeUS = Diagnostics.TotalScopeUS.load( std::memory_order_relaxed );
	const uint64_t TotalExecuteUS = Diagnostics.TotalExecuteUS.load( std::memory_order_relaxed );
	Result.MeanMutexWaitMs = Result.Queries ?
		static_cast<double>( TotalWaitUS ) / (1000.0 * Result.Queries) : 0.0;
	Result.MaxMutexWaitMs = static_cast<double>(
		Diagnostics.MaxWaitUS.load( std::memory_order_relaxed ) ) / 1000.0;
	Result.MeanScopeMs = Result.Queries ?
		static_cast<double>( TotalScopeUS ) / (1000.0 * Result.Queries) : 0.0;
	Result.MaxScopeMs = static_cast<double>(
		Diagnostics.MaxScopeUS.load( std::memory_order_relaxed ) ) / 1000.0;
	Result.MeanExecuteMs = Result.Queries ?
		static_cast<double>( TotalExecuteUS ) / (1000.0 * Result.Queries) : 0.0;
	Result.MaxExecuteMs = static_cast<double>(
		Diagnostics.MaxExecuteUS.load( std::memory_order_relaxed ) ) / 1000.0;
	{
		std::lock_guard Lock( Diagnostics.RecentMutex );
		Result.RecentSlowQueries.assign( Diagnostics.Recent.rbegin(), Diagnostics.Recent.rend() );
	}
	return Result;
}

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

void SQLiteDatabaseQuery::BindBlob( const char* paramName, const void* data, int bytes )
{
	Reset();

	for( auto& statement : m_statements )
	{
		int index = sqlite3_bind_parameter_index( statement, paramName );
		if( index )
		{
			int result = sqlite3_bind_blob( statement, index, data, bytes, SQLITE_TRANSIENT );
			AssertQuery( result == 0, "Failed to bind blob parameter: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
		}
	}
}

void SQLiteDatabaseQuery::BindNull( const char* paramName )
{
	Reset();

	for( auto& statement : m_statements )
	{
		int index = sqlite3_bind_parameter_index( statement, paramName );
		if( index )
		{
			int result = sqlite3_bind_null( statement, index );
			AssertQuery( result == 0, "Failed to bind null parameter: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
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
	auto Step = [this]( sqlite3_stmt* Statement )
	{
		const auto Started = std::chrono::steady_clock::now();
		const int Result = sqlite3_step( Statement );
		m_instanceExecuteUS += static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - Started ).count() );
		return Result;
	};

	m_reset = false;

	int count = 0;

	for( auto& statement : m_statements )
	{
		int result;
		bool earlyBreak = false;
		while( (result = Step( statement )) == SQLITE_ROW )
		{
			count++;

			if( callback && !callback(*this) )
			{
				earlyBreak = true;
				break;
			}
		}

		if( !earlyBreak )
		{
			AssertQuery( result == SQLITE_DONE, "Error while reading rows: %s", sqlite3_errmsg( m_database->GetDatabase() ) );
			if (result != SQLITE_DONE)
			{
				std::string ErrorString(sqlite3_errmsg( m_database->GetDatabase() ));


				m_lastError = std::string( ErrorString.begin(), ErrorString.end() );
				return -1;
			}
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

const void* SQLiteDatabaseQuery::GetColumnValueBlob( int column ) const
{
	return sqlite3_column_blob( m_statements.back(), column );
}

const int SQLiteDatabaseQuery::GetColumnValueBytes( int column ) const
{
	return sqlite3_column_bytes( m_statements.back(), column );
}

const int SQLiteDatabaseQuery::GetColumnCount() const
{
	return sqlite3_column_count( m_statements.back() );
}

SQLiteDatabase::SQLiteDatabase( const std::string& filename, const std::string& initScript, bool writeAccess, std::function<void(const std::string & )> onErrorCallback )
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

	std::string NewFilename = filename;

	int result = sqlite3_open_v2( NewFilename.c_str(), &m_database, flags, nullptr );
	AssertDB( result == 0, "Failed to open database: %s\n%s", NewFilename.c_str(), sqlite3_errmsg( m_database ) );

	// Allow up to 5 seconds for write contention before returning SQLITE_BUSY
	sqlite3_busy_timeout( m_database, 5000 );

	// Enable foreign key enforcement (required for ON DELETE CASCADE to work)
	sqlite3_exec( m_database, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr );

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

std::shared_ptr<SQLiteDatabaseQuery> SQLiteDatabase::CreateQuery( const std::string& queryName, const std::string& query )
{
	AssertDB( m_database, "Database was not valid" );

	auto generatedQuery = std::make_shared<SQLiteDatabaseQuery>( shared_from_this() );
	generatedQuery->SetQueryName( queryName );

	std::string newQuery = query;
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
