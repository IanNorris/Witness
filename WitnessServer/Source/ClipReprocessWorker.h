#pragma once

#include "WorkerBase.h"
#include "SQLite.h"

#include <ONNXDetectionFilter.h>

#include <functional>
#include <string>
#include <memory>

static constexpr int CURRENT_DETECTION_VERSION = 1;

class ClipReprocessWorker : public WorkerBase
{
public:

	ClipReprocessWorker(
		const std::shared_ptr<MessageBus>& MessageBus,
		std::shared_ptr<SQLiteDatabase> Database,
		std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter,
		std::string CachePath,
		std::function<bool()> IsIdle
	);

private:

	virtual void WorkerMain() override;

	std::shared_ptr<SQLiteDatabase> Database;
	std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter;
	std::string CachePath;
	std::function<bool()> IsIdle;
};
