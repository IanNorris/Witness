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
	: Mutex()
	, LongPoll(make_shared<LongPollDispatch>())
	{}

	CameraState* FindCameraById(int Id)
	{
		lock_guard<mutex> lock(Mutex);

		auto Iter = Cameras.find(Id);
		if (Iter != Cameras.end())
		{
			return &((*Iter).second);
		}
		else
		{
			return nullptr;
		}
	}

	const CameraState* FindCameraById(int Id) const
	{
		lock_guard<mutex> lock(Mutex);

		auto Iter = Cameras.find(Id);
		if (Iter != Cameras.end())
		{
			return &((*Iter).second);
		}
		else
		{
			return nullptr;
		}
	}

	const unordered_map<int, CameraState>& GetCameraMap() const
	{
		return Cameras;
	}

	unordered_map<int, CameraState>& GetCameraMap()
	{
		return Cameras;
	}

	mutable mutex Mutex;

	string_t CachePath;

	shared_ptr<SQLiteDatabase> Database;

	Witness::Camera::ImageProcessingJobQueue* CommonImageProcessingJobQueue;

	vector<SettingsMap> AzureSettings;

	shared_ptr<MessageBus> MessageBus;

	shared_ptr<AzureVisionAnalysisEndpointFilter> AzureVisionEndpoint;

	uint16_t Port;

	mutable shared_ptr<LongPollDispatch> LongPoll;

private:
	unordered_map< int, CameraState> Cameras;
};
