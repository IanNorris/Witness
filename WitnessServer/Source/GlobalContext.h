#pragma once

#include "Message.h"
#include "SQLite.h"
#include "cpprest/json.h"
#include "CameraState.h"
#include "CameraWorker.h"
#include "SettingsMap.h"
#include "LongPoll.h"
#include "Azure/AzureVisionAnalysisEndpointFilter.h"

struct CameraStateToggleRecordMessage : public Message
{
	CameraStateToggleRecordMessage( int CamIndex, bool RecordIn ) : Camera( CamIndex ), Record( RecordIn ) {}

	int Camera;
	bool Record;
};

class GlobalContext
{
public:

	GlobalContext()
	: LongPoll(make_shared<LongPollDispatch>())
	{}

	mutable mutex Mutex;

	string_t CachePath;

	shared_ptr<SQLiteDatabase> Database;
	unordered_map< int, CameraState> Cameras;
	Witness::Camera::ImageProcessingJobQueue* CommonImageProcessingJobQueue;

	vector<SettingsMap> AzureSettings;

	shared_ptr<MessageBus> MessageBus;

	shared_ptr<AzureVisionAnalysisEndpointFilter> AzureVisionEndpoint;

	uint16_t Port;

	mutable shared_ptr<LongPollDispatch> LongPoll;
};
