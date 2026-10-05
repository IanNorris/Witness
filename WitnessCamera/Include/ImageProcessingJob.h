#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "SourceStats.h"
#include "RecordFilter.h"

namespace Witness{
namespace Camera{

struct ImageProcessingJob;
struct ImageProcessingJobQueueData;

// Caller-owned POD buffer: no STL allocation or ownership crosses the DLL.
// Epoch and total sequence are returned in the same SourceStats snapshot.
struct ProcessingWorkSampleBuffer
{
    int64_t* Values = nullptr;
    size_t Capacity = 0;
    size_t Count = 0;
    uint64_t FirstSequence = 0;
};

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
	SourceStats GetStats( int SourceID, ProcessingWorkSampleBuffer* Samples );
	void ResetStats(int Source);

	void CompletedJob( int SourceID, uint64_t Generation, bool ReleaseAISlot );

	void WorkerThreadMain();

	void RequestShutdown();
};

}}
