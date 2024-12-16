#pragma once

namespace Database
{
	std::shared_ptr<SQLiteDatabase> InitializeDatabase( string_t Filename );
}