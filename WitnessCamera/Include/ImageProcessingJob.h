#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "SourceStats.h"

namespace Witness{
namespace Camera{

struct ImageProcessingJob;
struct ImageProcessingJobQueueData;

struct CAMERA_API ImageProcessingJobQueue : public Pimpl<ImageProcessingJobQueueData>
{
	bool Push(const std::shared_ptr<ImageProcessingJob>& Job);
	bool TryPop(std::shared_ptr<ImageProcessingJob>& Job);
	void Pop(std::shared_ptr<ImageProcessingJob>& Job);
	void RemoveAllForSource( int SourceID );

	SourceStats GetStats( int SourceID );

	void CompletedJob( int SourceID );

	void WorkerThreadMain();
};

}}