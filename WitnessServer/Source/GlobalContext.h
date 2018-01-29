#pragma once

#include "SQLite.h"
#include "CameraWorker.h"
#include "cpprest/json.h"

class GlobalContext
{
public:

	mutable mutex Mutex;

	shared_ptr<SQLiteDatabase> Database;
	unordered_map< int, shared_ptr<CameraWorker> > CameraWorkers;
	unordered_map< int, string_t > CameraNames;
	shared_ptr<MessageBus> MessageBus;

	unordered_map< int, vector<unsigned char> > CameraPreviews;
};
