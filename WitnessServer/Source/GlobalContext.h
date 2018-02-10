#pragma once

#include "SQLite.h"
#include "CameraWorker.h"
#include "cpprest/json.h"

struct CameraStateToggleRecordMessage : public Message
{
	CameraStateToggleRecordMessage( int CamIndex, bool RecordIn ) : Camera( CamIndex ), Record( RecordIn ) {}

	int Camera;
	bool Record;
};

class GlobalContext
{
public:
	
	struct CameraState
	{
		CameraState()
		: IsRecording(false)
		, IsManualRecording(false)
		{

		}

		shared_ptr<CameraWorker> Worker;
		string_t Name;

		vector<unsigned char> PreviewThumbnail;

		unordered_map< uint64_t, vector<unsigned char> > ClipThumbnails;

		bool IsRecording;
		bool IsManualRecording;
	};

	mutable mutex Mutex;

	string_t CachePath;

	shared_ptr<SQLiteDatabase> Database;
	unordered_map< int, CameraState> Cameras;

	shared_ptr<MessageBus> MessageBus;
};
