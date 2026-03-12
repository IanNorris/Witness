#pragma once

#include "WorkerBase.h"
#include "SQLite.h"
#include "FaceRecognitionCache.h"

#include <ONNXDetectionFilter.h>
#include <FaceDetectionFilter.h>
#include <FaceEmbeddingModel.h>

#include <functional>
#include <string>
#include <memory>

static constexpr int CURRENT_DETECTION_VERSION = 12;

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
		double FaceRecThreshold,
		std::string CachePath,
		std::function<bool()> IsIdle
	);

private:

	virtual void WorkerMain() override;
	void ProcessClip( int64_t clipUID, int64_t timestamp, int camera, int recordMode, const std::string& existingTags );
	void BackfillLighting();

	std::shared_ptr<SQLiteDatabase> Database;
	std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter;
	std::shared_ptr<Witness::Camera::FaceDetectionFilter> FaceFilter;
	std::shared_ptr<Witness::Camera::FaceEmbeddingModel> FaceEmbModel;
	std::shared_ptr<FaceRecognitionCache> FaceCache;
	double FaceRecThreshold;
	std::string CachePath;
	std::function<bool()> IsIdle;
	bool LightingBackfillComplete = false;
};
