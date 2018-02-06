#pragma once

#include "SQLite.h"
#include "CameraWorker.h"
#include "cpprest/json.h"

class GlobalContext
{
public:

	mutable mutex Mutex;

	string_t CachePath;

	shared_ptr<SQLiteDatabase> Database;
	unordered_map< int, shared_ptr<CameraWorker> > CameraWorkers;
	unordered_map< int, string_t > CameraNames;
	shared_ptr<MessageBus> MessageBus;

	unordered_map< int, vector<unsigned char> > CameraPreviews;
	unordered_map< int, vector<unsigned char> > CameraMotionMask;

	unordered_map< int, unordered_map< uint64_t, vector<unsigned char> > > CameraFrames;
};
