#pragma once

#include "SQLite.h"

namespace Database
{
	std::shared_ptr<SQLiteDatabase> InitializeDatabase( std::string Filename );
	bool HasAdminUser( const std::shared_ptr<SQLiteDatabase>& DB );
}