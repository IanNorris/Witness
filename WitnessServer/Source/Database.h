#pragma once

namespace Database
{
	shared_ptr<SQLiteDatabase> InitializeDatabase( string_t Filename );
}