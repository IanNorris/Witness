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
	: LongPoll(std::make_shared<LongPollDispatch>())
	{}

	mutable std::mutex Mutex;

	string_t CachePath;

	std::shared_ptr<SQLiteDatabase> Database;
	std::unordered_map< int, CameraState> Cameras;
	Witness::Camera::ImageProcessingJobQueue* CommonImageProcessingJobQueue;

	std::vector<SettingsMap> AzureSettings;

	std::shared_ptr<MessageBus> MessageBus;

	std::shared_ptr<AzureVisionAnalysisEndpointFilter> AzureVisionEndpoint;

	uint16_t Port;

	mutable std::shared_ptr<LongPollDispatch> LongPoll;
};
