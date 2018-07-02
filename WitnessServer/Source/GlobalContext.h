#pragma once

#include "SQLite.h"
#include "CameraWorker.h"
#include "cpprest/json.h"
#include "CameraState.h"

struct CameraStateToggleRecordMessage : public Message
{
	CameraStateToggleRecordMessage( int CamIndex, bool RecordIn ) : Camera( CamIndex ), Record( RecordIn ) {}

	int Camera;
	bool Record;
};

class GlobalContext
{
public:

	mutable mutex Mutex;

	string_t CachePath;

	shared_ptr<SQLiteDatabase> Database;
	unordered_map< int, CameraState> Cameras;
	Witness::Camera::ImageProcessingJobQueue* CommonImageProcessingJobQueue;

	shared_ptr<MessageBus> MessageBus;
};
