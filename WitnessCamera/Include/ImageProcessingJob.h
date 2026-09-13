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
	void SetMaximumConcurrentAIJobs( size_t MaximumJobs );
	bool HasLiveAIWork();
	bool IsCurrentJob( const SharedClassificationTask& Job );
	bool TryAcquireBackgroundAIJob();
	void CompletedBackgroundAIJob();

	SourceStats GetStats( int SourceID );
	void ResetStats(int Source);

	void CompletedJob( int SourceID, uint64_t Generation, bool ReleaseAISlot );

	void WorkerThreadMain();

	void RequestShutdown();
};

}}
