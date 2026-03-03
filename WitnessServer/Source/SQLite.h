#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include "Common.h"
#include "sqlite3.h"

class SQLiteDatabase;

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

	void Reset();

	int Execute( const std::function< bool(const SQLiteDatabaseQuery&) >& callback );

	const char* GetColumnValueText( int column ) const;
	sqlite3_value* GetColumnValue( int column ) const;
	const int GetColumnValueInt( int column ) const;
	const int64_t GetColumnValueInt64( int column ) const;
	const double GetColumnValueDouble( int column ) const;
	const int GetColumnCount() const;

	inline int64_t GetLastInsertionId(){ return m_lastInsertId; }

	std::mutex& PrepareQueryMutex() { return m_tMutex; }

	std::string GetLastError() { return m_lastError; }

	void SetQueryName( const std::string& name ) { m_queryName = name; }
	const std::string& GetQueryName() const { return m_queryName; }

private:

	std::mutex												m_tMutex;

	std::string												m_queryName;
	std::string												m_lastError;
	std::shared_ptr<SQLiteDatabase>							m_database;
	std::vector<sqlite3_stmt*>								m_statements;
	int64_t													m_lastInsertId;
	bool													m_reset;
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
	SQLiteDatabaseQueryInstance( const std::shared_ptr<SQLiteDatabase>& DB, const char* QueryName )
	: m_Query( DB->GetQuery( QueryName ) )
	, m_Lock( m_Query->PrepareQueryMutex() )
	{
	}

	std::shared_ptr<SQLiteDatabaseQuery> operator ->()
	{
		return m_Query;
	}

private:

	std::shared_ptr<SQLiteDatabaseQuery> m_Query;
	std::lock_guard<std::mutex> m_Lock;
};
