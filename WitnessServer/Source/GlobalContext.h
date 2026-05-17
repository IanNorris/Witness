#pragma once

#include "Message.h"
#include "SQLite.h"
#include "CameraState.h"
#include "CameraWorker.h"
#include "SettingsMap.h"
#include "LongPoll.h"
#include "EventBroadcaster.h"
#include "StreamBroadcaster.h"

#include "FaceRecognitionCache.h"
#include "SoundManager.h"

#include <shared_mutex>

namespace Witness{ namespace Camera{ class FaceEmbeddingModel; class ReolinkBaichuanClient; } }
class ReolinkClient;

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
	, Streams(std::make_shared<StreamBroadcaster>())
	{}

	CameraState* FindCameraById(int Id)
	{
		std::shared_lock<std::shared_mutex> lock(Mutex);

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
		std::shared_lock<std::shared_mutex> lock(Mutex);

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

	mutable std::shared_mutex Mutex;

	std::string CachePath;

	std::shared_ptr<SQLiteDatabase> Database;

	Witness::Camera::ImageProcessingJobQueue* CommonImageProcessingJobQueue;

	std::shared_ptr<MessageBus> MessageBus;

	uint16_t Port;

	mutable std::shared_ptr<LongPollDispatch> LongPoll;

	std::shared_ptr<EventBroadcaster> Events;

	std::shared_ptr<StreamBroadcaster> Streams;

	std::string BuildHash;

	// Face recognition
	std::shared_ptr<Witness::Camera::FaceEmbeddingModel> FaceEmbeddingModel;
	std::shared_ptr<FaceRecognitionCache> FaceCache;

	// Sound actions
	std::shared_ptr<SoundManager> Sound;

	// PTZ clients (keyed by CameraUID)
	std::unordered_map<int, std::shared_ptr<ReolinkClient>> PtzClients;
	std::shared_ptr<ReolinkClient> GetPtzClient(int cameraId)
	{
		std::shared_lock<std::shared_mutex> lock(Mutex);
		auto it = PtzClients.find(cameraId);
		return (it != PtzClients.end()) ? it->second : nullptr;
	}

	// Baichuan clients for PTZ-over-Baichuan (keyed by CameraUID)
	std::unordered_map<int, std::shared_ptr<Witness::Camera::ReolinkBaichuanClient>> BaichuanClients;
	std::shared_ptr<Witness::Camera::ReolinkBaichuanClient> GetBaichuanClient(int cameraId)
	{
		std::shared_lock<std::shared_mutex> lock(Mutex);
		auto it = BaichuanClients.find(cameraId);
		return (it != BaichuanClients.end()) ? it->second : nullptr;
	}

private:
	std::unordered_map< int, CameraState> Cameras;
};
