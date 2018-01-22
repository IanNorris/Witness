#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include "Common.h"
#include "sqlite3.h"

using namespace std;

class SQLiteDatabase;

class SQLiteDatabaseQuery
{
public:
	SQLiteDatabaseQuery( shared_ptr<SQLiteDatabase> database );
	~SQLiteDatabaseQuery();

	void AddStatement( sqlite3_stmt* statement )
	{
		m_statements.push_back( statement );
	}

	void Bind( const char* parameterName, const TCHAR* value );
	void Bind( const char* parameterName, double value );
	void Bind( const char* parameterName, int value );

	void Reset();

	int Execute( const std::function< bool(const SQLiteDatabaseQuery&) >& callback );

	const wchar_t* GetColumnValueText( int column ) const;
	sqlite3_value* GetColumnValue( int column ) const;
	const int GetColumnValueInt( int column ) const;
	const int64_t GetColumnValueInt64( int column ) const;
	const double GetColumnValueDouble( int column ) const;
	const int GetColumnCount() const;

	inline int64_t GetLastInsertionId(){ return m_lastInsertId; }

	mutex& PrepareQueryMutex() { return m_tMutex; }

private:

	mutex													m_tMutex;

	shared_ptr<SQLiteDatabase>								m_database;
	vector<sqlite3_stmt*>									m_statements;
	int64_t													m_lastInsertId;
	bool													m_reset;
};

class SQLiteDatabase : public enable_shared_from_this<SQLiteDatabase>
{
public:

	SQLiteDatabase( const string_t& filename, const string& initScript, bool writeAccess );
	~SQLiteDatabase();

	inline sqlite3*	GetDatabase() { return m_database; };

	void Initialise( void );

	shared_ptr<SQLiteDatabaseQuery> CreateQuery( const string_t& queryName, const string_t& query );

	const shared_ptr<SQLiteDatabaseQuery>& GetQuery(const string_t& queryName) { return m_queries[queryName]; }

	bool IsNewlyCreated() const { return m_databaseNewlyCreated; }

private:

	unordered_map<string_t, shared_ptr<SQLiteDatabaseQuery>>	m_queries;

	string_t		m_filename;
	sqlite3*	m_database;
	bool		m_databaseNewlyCreated;
};

class SQLiteDatabaseQueryInstance
{
public:
	SQLiteDatabaseQueryInstance( const shared_ptr<SQLiteDatabase>& DB, const TCHAR* QueryName )
	: m_Query( DB->GetQuery( QueryName ) )
	, m_Lock( m_Query->PrepareQueryMutex() )
	{
	}

	shared_ptr<SQLiteDatabaseQuery> operator ->()
	{
		return m_Query;
	}

private:

	shared_ptr<SQLiteDatabaseQuery> m_Query;
	lock_guard<mutex> m_Lock;
};
