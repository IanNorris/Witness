#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "SourceStats.h"
#include "RecordFilter.h"

namespace Witness{
namespace Camera{

struct ImageProcessingJob;
struct ImageProcessingJobQueueData;

struct CAMERA_API ImageProcessingJobQueue : public Pimpl<ImageProcessingJobQueueData>
{
	bool Push(const SharedClassificationTask& Job, bool HighPriority);
	bool TryPop(SharedClassificationTask& Job);
	void Pop(SharedClassificationTask& Job);
	void RemoveAllForSource( int SourceID );

	SourceStats GetStats( int SourceID );
	void ResetStats(int Source);

	void CompletedJob( int SourceID );

	void WorkerThreadMain();
};

}}