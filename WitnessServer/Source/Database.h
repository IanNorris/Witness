#pragma once

namespace Database
{
	std::shared_ptr<SQLiteDatabase> InitializeDatabase( StringT Filename );
}