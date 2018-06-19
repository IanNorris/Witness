#pragma once

#include "WorkerBase.h"
#include <ImageProcessingJob.h>

class ObservingMotionFilter;

using namespace Witness::Camera;

class ImageProcessorWorker : public WorkerBase
{
public:
	ImageProcessorWorker(const shared_ptr<MessageBus>& MessageBus, ImageProcessingJobQueue* JobQueue)
	: WorkerBase( MessageBus )
	, JobQueue( JobQueue )
	{}

private:

	virtual void WorkerMain() override;

	ImageProcessingJobQueue* JobQueue;
};
