#pragma once

#include "WorkerBase.h"
#include "SQLite.h"
#include "FaceRecognitionCache.h"
#include "EventBroadcaster.h"

#include <ONNXDetectionFilter.h>
#include <FaceDetectionFilter.h>
#include <FaceEmbeddingModel.h>

#include <functional>
#include <string>
#include <memory>

static constexpr int CURRENT_DETECTION_VERSION = 16;

class ClipReprocessWorker : public WorkerBase
{
public:

	ClipReprocessWorker(
		const std::shared_ptr<MessageBus>& MessageBus,
		std::shared_ptr<SQLiteDatabase> Database,
		std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter,
		std::shared_ptr<Witness::Camera::FaceDetectionFilter> FaceFilter,
		std::shared_ptr<Witness::Camera::FaceEmbeddingModel> FaceEmbModel,
		std::shared_ptr<FaceRecognitionCache> FaceCache,
		std::shared_ptr<EventBroadcaster> Events,
		double FaceRecThreshold,
		double DetectionMaxFPS,
		std::string CachePath,
		std::function<bool()> IsIdle
	);

private:

	virtual void WorkerMain() override;
	void ProcessClip( int64_t clipUID, int64_t timestamp, int camera, int recordMode, const std::string& existingTags,
		int queuePosition, int queueTotal );
	void BroadcastProgress( int64_t clipUID, const std::string& stage, int frame, int totalFrames, int queuePos, int queueTotal,
		const std::string& tags = "", int lighting = -1 );
	void BackfillLighting();
	void ComputeAndStoreTrails( int64_t clipUID, int cameraID, double fromTime, double toTime );

	std::shared_ptr<SQLiteDatabase> Database;
	std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter;
	std::shared_ptr<Witness::Camera::FaceDetectionFilter> FaceFilter;
	std::shared_ptr<Witness::Camera::FaceEmbeddingModel> FaceEmbModel;
	std::shared_ptr<FaceRecognitionCache> FaceCache;
	std::shared_ptr<EventBroadcaster> Events;
	double FaceRecThreshold;
	double DetectionMaxFPS;
	std::string CachePath;
	std::function<bool()> IsIdle;
	bool LightingBackfillComplete = false;
};
