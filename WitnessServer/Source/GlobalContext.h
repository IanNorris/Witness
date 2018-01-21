#pragma once

#include "SQLite.h"

#include "cpprest/json.h"

class GlobalContext
{
public:

	shared_ptr<SQLiteDatabase> Database;

};
