#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <string>
#include "Common.h"
#include "sqlite3.h"

class SQLiteDatabase;

struct SQLiteQueryTimingEvent
{
	uint64_t Sequence = 0;
	int64_t StartedUnixMs = 0;
	std::string Query;
	uint64_t Thread = 0;
	double MutexWaitMs = 0.0;
	double ScopeMs = 0.0;
	double ExecuteMs = 0.0;
};

struct SQLiteDiagnosticsSnapshot
{
	uint64_t Queries = 0;
	uint64_t ContendedQueries = 0;
	uint64_t SlowQueries = 0;
	double MeanMutexWaitMs = 0.0;
	double MaxMutexWaitMs = 0.0;
	double MeanScopeMs = 0.0;
	double MaxScopeMs = 0.0;
	double MeanExecuteMs = 0.0;
	double MaxExecuteMs = 0.0;
	std::vector<SQLiteQueryTimingEvent> RecentSlowQueries;
};

SQLiteDiagnosticsSnapshot GetSQLiteDiagnosticsSnapshot();

#define MAKE_QUERY( Name ) SQLiteDatabaseQueryInstance Name( Context->Database, #Name )

class SQLiteDatabaseQuery
{
public:
	SQLiteDatabaseQuery(std::shared_ptr<SQLiteDatabase> database );
	~SQLiteDatabaseQuery();

	void AddStatement( sqlite3_stmt* statement )
	{
		m_statements.push_back( statement );
	}

	void Bind( const char* parameterName, const char* value );
	void Bind( const char* parameterName, double value );
	void Bind( const char* parameterName, int value );
	void Bind( const char* parameterName, int64_t value );
	void BindBlob( const char* parameterName, const void* data, int bytes );
	void BindNull( const char* parameterName );

	void Reset();

	int Execute( const std::function< bool(const SQLiteDatabaseQuery&) >& callback );

	const char* GetColumnValueText( int column ) const;
	sqlite3_value* GetColumnValue( int column ) const;
	const int GetColumnValueInt( int column ) const;
	const int64_t GetColumnValueInt64( int column ) const;
	const double GetColumnValueDouble( int column ) const;
	const void* GetColumnValueBlob( int column ) const;
	const int GetColumnValueBytes( int column ) const;
	const int GetColumnCount() const;

	inline int64_t GetLastInsertionId(){ return m_lastInsertId; }

	std::mutex& PrepareQueryMutex() { return m_tMutex; }

	std::string GetLastError() { return m_lastError; }

	void SetQueryName( const std::string& name ) { m_queryName = name; }
	const std::string& GetQueryName() const { return m_queryName; }
	void ResetInstanceExecuteTime() { m_instanceExecuteUS = 0; }
	uint64_t GetInstanceExecuteTimeUS() const { return m_instanceExecuteUS; }

private:

	std::mutex												m_tMutex;

	std::string												m_queryName;
	std::string												m_lastError;
	std::shared_ptr<SQLiteDatabase>							m_database;
	std::vector<sqlite3_stmt*>								m_statements;
	int64_t													m_lastInsertId;
	bool													m_reset;
	uint64_t										m_instanceExecuteUS = 0;
};

class SQLiteDatabase : public std::enable_shared_from_this<SQLiteDatabase>
{
public:

	SQLiteDatabase( const std::string& filename, const std::string& initScript, bool writeAccess, std::function<void(const std::string&)> onErrorCallback );
	~SQLiteDatabase();

	inline sqlite3*	GetDatabase() { return m_database; };

	void Initialise( void );

	std::shared_ptr<SQLiteDatabaseQuery> CreateQuery( const std::string& queryName, const std::string& query );

	const std::shared_ptr<SQLiteDatabaseQuery>& GetQuery(const std::string& queryName) { return m_queries[queryName]; }

	bool IsNewlyCreated() const { return m_databaseNewlyCreated; }

	void ThrowError( const std::string& Message );

private:

	std::unordered_map<std::string, std::shared_ptr<SQLiteDatabaseQuery>>	m_queries;

	std::function<void(const std::string&)>								m_onErrorCallback;

	std::string		m_filename;
	sqlite3*	m_database;
	bool		m_databaseNewlyCreated;
};

class SQLiteDatabaseQueryInstance
{
public:
	SQLiteDatabaseQueryInstance( const std::shared_ptr<SQLiteDatabase>& DB, const char* QueryName );
	~SQLiteDatabaseQueryInstance() noexcept;

	SQLiteDatabaseQueryInstance( const SQLiteDatabaseQueryInstance& ) = delete;
	SQLiteDatabaseQueryInstance& operator=( const SQLiteDatabaseQueryInstance& ) = delete;

	std::shared_ptr<SQLiteDatabaseQuery> operator ->()
	{
		return m_Query;
	}

private:

	std::shared_ptr<SQLiteDatabaseQuery> m_Query;
	std::unique_lock<std::mutex> m_Lock;
	std::chrono::steady_clock::time_point m_WaitStarted;
	std::chrono::steady_clock::time_point m_LockAcquired;
	std::chrono::system_clock::time_point m_StartedSystem;
	uint64_t m_MutexWaitUS = 0;
};
