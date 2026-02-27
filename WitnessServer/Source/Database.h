#pragma once

#include "SQLite.h"

namespace Database
{
	std::shared_ptr<SQLiteDatabase> InitializeDatabase( StringT Filename );
}