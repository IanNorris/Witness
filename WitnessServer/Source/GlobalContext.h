#pragma once

#include "Message.h"
#include "SQLite.h"
#include "CameraState.h"
#include "CameraWorker.h"
#include "SettingsMap.h"
#include "LongPoll.h"
#include "EventBroadcaster.h"

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
	, LongPoll(std::make_shared<LongPollDispatch>())
	, Events(std::make_shared<EventBroadcaster>())
	{}

	CameraState* FindCameraById(int Id)
	{
		std::lock_guard<std::mutex> lock(Mutex);

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
		std::lock_guard<std::mutex> lock(Mutex);

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

	const std::unordered_map<int, CameraState>& GetCameraMap() const
	{
		return Cameras;
	}

	std::unordered_map<int, CameraState>& GetCameraMap()
	{
		return Cameras;
	}

	mutable std::mutex Mutex;

	std::string CachePath;

	std::shared_ptr<SQLiteDatabase> Database;

	Witness::Camera::ImageProcessingJobQueue* CommonImageProcessingJobQueue;

	std::shared_ptr<MessageBus> MessageBus;

	uint16_t Port;

	mutable std::shared_ptr<LongPollDispatch> LongPoll;

	std::shared_ptr<EventBroadcaster> Events;

private:
	std::unordered_map< int, CameraState> Cameras;
};
