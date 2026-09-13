#include "FFMPEG/Frame.h"
#include "FFMPEG/Common.h"
#include "ImageProcessingData.h"
#include "ImageProcessingJob.h"
#include "MotionFilter.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

#include <windows.h>
#include <minmax.h>
#include <algorithm>

namespace Witness{
namespace Camera{

PIMPL_CONSTRUCT(ImageProcessingJobQueueData)

namespace
{
enum class QueueMetricKind
{
	Ingress,
	Continuation,
	AI,
};

int64_t QueueClockNowNS()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsSourceActive(const std::vector<int>& Sources, int SourceID)
{
	return std::find(Sources.begin(), Sources.end(), SourceID) != Sources.end();
}

bool IsOptionalAIJob(const SharedClassificationTask& Job)
{
	return Job && Job->Next && Job->Next->GetWorkClass() == EFilterWorkClass::OptionalAI;
}

uint64_t CurrentGeneration(ImageProcessingJobQueueData& Data, int SourceID)
{
	auto& Generation = Data.SourceGenerations[SourceID];
	if (Generation == 0)
		Generation = 1;
	return Generation;
}

bool IsCurrentGeneration(ImageProcessingJobQueueData& Data, const SharedClassificationTask& Job)
{
	return Job && Job->QueueGeneration == CurrentGeneration(Data, Job->Frame.SourceID);
}

void EnsureEssentialSource(ImageProcessingJobQueueData& Data, int SourceID)
{
	if (!IsSourceActive(Data.EssentialSourceOrder, SourceID))
		Data.EssentialSourceOrder.push_back(SourceID);
}

void RecordQueueAdmission(ImageProcessingJobQueueData& Data, int SourceID, QueueMetricKind Kind, bool Coalesced)
{
	std::lock_guard<std::mutex> Lock(Data.StatsMutex);
	auto& Stats = Data.Stats[SourceID];
	if (Kind == QueueMetricKind::Ingress)
	{
		++Stats.IngressFrames;
		if (Coalesced)
			++Stats.CoalescedFrames;
		else
			++Stats.PendingEssentialJobs;
		Stats.PeakPendingEssentialJobs = (std::max)(Stats.PeakPendingEssentialJobs, Stats.PendingEssentialJobs);
	}
	else if (Kind == QueueMetricKind::Continuation)
	{
		++Stats.PendingEssentialJobs;
		Stats.PeakPendingEssentialJobs = (std::max)(Stats.PeakPendingEssentialJobs, Stats.PendingEssentialJobs);
	}
	else
	{
		if (Coalesced)
			++Stats.CoalescedAIFrames;
		else
			++Stats.PendingAIJobs;
		Stats.PeakPendingAIJobs = (std::max)(Stats.PeakPendingAIJobs, Stats.PendingAIJobs);
	}
}

void RecordQueueRemoval(ImageProcessingJobQueueData& Data, int SourceID, QueueMetricKind Kind)
{
	std::lock_guard<std::mutex> Lock(Data.StatsMutex);
	auto& Stats = Data.Stats[SourceID];
	uint64_t& Pending = Kind == QueueMetricKind::AI ? Stats.PendingAIJobs : Stats.PendingEssentialJobs;
	if (Pending > 0)
		--Pending;
}

void RecordQueueDispatch(ImageProcessingJobQueueData& Data, const SharedClassificationTask& Job, QueueMetricKind Kind)
{
	const int64_t WaitNS = (std::max)(int64_t{0}, QueueClockNowNS() - Job->QueueEnteredTimestampNS);
	std::lock_guard<std::mutex> Lock(Data.StatsMutex);
	auto& Stats = Data.Stats[Job->Frame.SourceID];
	uint64_t& Pending = Kind == QueueMetricKind::AI ? Stats.PendingAIJobs : Stats.PendingEssentialJobs;
	if (Pending > 0)
		--Pending;

	uint64_t* Samples = nullptr;
	int64_t* Total = nullptr;
	int64_t* Maximum = nullptr;
	if (Kind == QueueMetricKind::Ingress)
	{
		++Stats.StartedFrames;
		Samples = &Stats.IngressQueueWaitSamples;
		Total = &Stats.IngressQueueWaitTotalNS;
		Maximum = &Stats.IngressQueueWaitMaxNS;
	}
	else if (Kind == QueueMetricKind::Continuation)
	{
		Samples = &Stats.ContinuationQueueWaitSamples;
		Total = &Stats.ContinuationQueueWaitTotalNS;
		Maximum = &Stats.ContinuationQueueWaitMaxNS;
	}
	else
	{
		Samples = &Stats.AIQueueWaitSamples;
		Total = &Stats.AIQueueWaitTotalNS;
		Maximum = &Stats.AIQueueWaitMaxNS;
	}

	++(*Samples);
	*Total += WaitNS;
	*Maximum = (std::max)(*Maximum, WaitNS);
}

bool TakeRunnableEssentialJob(ImageProcessingJobQueueData& Data, SharedClassificationTask& Job)
{
	const size_t SourceCount = Data.EssentialSourceOrder.size();
	if (SourceCount == 0)
		return false;

	for (size_t Offset = 0; Offset < SourceCount; ++Offset)
	{
		const size_t SourceIndex = (Data.NextEssentialSource + Offset) % SourceCount;
		const int SourceID = Data.EssentialSourceOrder[SourceIndex];
		if (IsSourceActive(Data.ActiveSources, SourceID))
			continue;

		auto Continuation = std::find_if(Data.HighPriorityAsyncQueue.begin(), Data.HighPriorityAsyncQueue.end(),
			[SourceID](const SharedClassificationTask& Candidate)
			{
				return Candidate && Candidate->Frame.SourceID == SourceID;
			});
		if (Continuation != Data.HighPriorityAsyncQueue.end())
		{
			Job = *Continuation;
			Data.HighPriorityAsyncQueue.erase(Continuation);
			RecordQueueDispatch(Data, Job, QueueMetricKind::Continuation);
		}
		else
		{
			auto Ingress = std::find_if(Data.Queue.begin(), Data.Queue.end(),
				[SourceID](const SharedClassificationTask& Candidate)
				{
					return Candidate && Candidate->Frame.SourceID == SourceID;
				});
			if (Ingress == Data.Queue.end())
				continue;
			Job = *Ingress;
			Data.Queue.erase(Ingress);
			RecordQueueDispatch(Data, Job, QueueMetricKind::Ingress);
		}

		Data.ActiveSources.push_back(SourceID);
		Data.NextEssentialSource = (SourceIndex + 1) % SourceCount;
		return true;
	}
	return false;
}

bool TakeRunnableAIJob(ImageProcessingJobQueueData& Data, std::vector<SharedClassificationTask>& Queue, bool RequireAvailableSlot, SharedClassificationTask& Job)
{
	for (auto Iter = Queue.begin(); Iter != Queue.end(); )
	{
		if (!IsCurrentGeneration(Data, *Iter))
		{
			RecordQueueRemoval(Data, (*Iter)->Frame.SourceID, QueueMetricKind::AI);
			Iter = Queue.erase(Iter);
			continue;
		}

		if (IsSourceActive(Data.ActiveSources, (*Iter)->Frame.SourceID))
		{
			++Iter;
			continue;
		}

		const int SourceID = (*Iter)->Frame.SourceID;
		const bool HasAISlot = IsSourceActive(Data.ActiveAISources, SourceID);
		if (!RequireAvailableSlot && !HasAISlot)
		{
			RecordQueueRemoval(Data, SourceID, QueueMetricKind::AI);
			Iter = Queue.erase(Iter);
			continue;
		}
		if (RequireAvailableSlot && !HasAISlot &&
			Data.ActiveAISources.size() + Data.ActiveBackgroundAIJobs >= Data.MaximumConcurrentAIJobs)
		{
			++Iter;
			continue;
		}

		Data.ActiveSources.push_back(SourceID);
		Job = *Iter;
		Queue.erase(Iter);
		RecordQueueDispatch(Data, Job, QueueMetricKind::AI);
		if (!HasAISlot)
			Data.ActiveAISources.push_back(SourceID);
		Job->HoldsAIReservation = true;
		return true;
	}
	return false;
}

void RemoveSourceJobs(ImageProcessingJobQueueData& Data, std::vector<SharedClassificationTask>& Queue, int SourceID, QueueMetricKind Kind)
{
	for (auto Iter = Queue.begin(); Iter != Queue.end(); )
	{
		if (*Iter && (*Iter)->Frame.SourceID == SourceID)
		{
			RecordQueueRemoval(Data, SourceID, Kind);
			Iter = Queue.erase(Iter);
		}
		else
			++Iter;
	}
}
}

bool ImageProcessingJobQueue::Push(const SharedClassificationTask& Job, bool HighPriority)
{
	auto& ID = *m_InternalData;
	std::unique_lock<std::mutex> Lock(ID.QueueMutex);

	if (Job)
	{
		if (Job->QueueGeneration == 0)
			Job->QueueGeneration = CurrentGeneration(ID, Job->Frame.SourceID);
		else if (!IsCurrentGeneration(ID, Job))
			return true;
		Job->QueueEnteredTimestampNS = QueueClockNowNS();
	}

	if (IsOptionalAIJob(Job))
	{
		if (Job->HoldsAIReservation)
		{
			// A continuation carries results from work already performed. It must
			// reach the observer rather than being replaced by a newer candidate.
			ID.AIContinuationQueue.push_back(Job);
			RecordQueueAdmission(ID, Job->Frame.SourceID, QueueMetricKind::AI, false);
			ID.Condition.notify_one();
			return true;
		}

		// Keep only the freshest optional AI frame for each camera.
		for (auto& PendingJob : ID.AIQueue)
		{
			if (PendingJob->Frame.SourceID == Job->Frame.SourceID)
			{
				// The replaced candidate completed essential processing, so retain
				// its filter timings even though optional inference is skipped.
				ID.AddFrame(PendingJob->Frame.SourceID, PendingJob->Frame.Timestamp, PendingJob->Frame.Stats);
				PendingJob = Job;
				RecordQueueAdmission(ID, Job->Frame.SourceID, QueueMetricKind::AI, true);
				ID.Condition.notify_one();
				return true;
			}
		}
		ID.AIQueue.push_back(Job);
		RecordQueueAdmission(ID, Job->Frame.SourceID, QueueMetricKind::AI, false);
	}
	else if (HighPriority)
	{
		// A null high-priority job is the shutdown sentinel.
		ID.HighPriorityAsyncQueue.push_back(Job);
		if (Job)
		{
			EnsureEssentialSource(ID, Job->Frame.SourceID);
			RecordQueueAdmission(ID, Job->Frame.SourceID, QueueMetricKind::Continuation, false);
		}
	}
	else
	{
		EnsureEssentialSource(ID, Job->Frame.SourceID);
		for (auto& PendingJob : ID.Queue)
		{
			if (PendingJob->Frame.SourceID == Job->Frame.SourceID)
			{
				PendingJob = Job;
				RecordQueueAdmission(ID, Job->Frame.SourceID, QueueMetricKind::Ingress, true);
				ID.Condition.notify_one();
				return true;
			}
		}
		ID.Queue.push_back(Job);
		RecordQueueAdmission(ID, Job->Frame.SourceID, QueueMetricKind::Ingress, false);
	}

	ID.Condition.notify_one();
	return true;
}

bool ImageProcessingJobQueue::TryPop(SharedClassificationTask& Job)
{
	auto& ID = *m_InternalData;
	std::unique_lock<std::mutex> Lock(ID.QueueMutex);

	// Essential continuations and new motion work always win. A blocked
	// high-priority job must not hide a runnable normal job.
	if (TakeRunnableEssentialJob(ID, Job))
		return true;

	if (TakeRunnableAIJob(ID, ID.AIContinuationQueue, false, Job) ||
		TakeRunnableAIJob(ID, ID.AIQueue, true, Job))
	{
		return true;
	}
	return false;
}

void ImageProcessingJobQueue::Pop(SharedClassificationTask& Job)
{
	auto& ID = *m_InternalData;
	std::unique_lock<std::mutex> Lock(ID.QueueMutex);

	while (!ID.WantExit)
	{
		if (std::find(ID.HighPriorityAsyncQueue.begin(), ID.HighPriorityAsyncQueue.end(), nullptr) != ID.HighPriorityAsyncQueue.end())
		{
			ID.WantExit = true;
			break;
		}

		if (TakeRunnableEssentialJob(ID, Job))
			return;

		if (TakeRunnableAIJob(ID, ID.AIContinuationQueue, false, Job) ||
			TakeRunnableAIJob(ID, ID.AIQueue, true, Job))
		{
			return;
		}

		ID.Condition.wait(Lock);
	}
}

void ImageProcessingJobQueue::RemoveAllForSource(int SourceID)
{
	auto& ID = *m_InternalData;
	std::unique_lock<std::mutex> Lock(ID.QueueMutex);
	RemoveSourceJobs(ID, ID.Queue, SourceID, QueueMetricKind::Ingress);
	RemoveSourceJobs(ID, ID.HighPriorityAsyncQueue, SourceID, QueueMetricKind::Continuation);
	RemoveSourceJobs(ID, ID.AIQueue, SourceID, QueueMetricKind::AI);
	RemoveSourceJobs(ID, ID.AIContinuationQueue, SourceID, QueueMetricKind::AI);
	ID.SourceGenerations[SourceID] = CurrentGeneration(ID, SourceID) + 1;

	// If inference completed and its continuation was waiting, no worker will
	// remain to release the reservation after the purge.
	if (!IsSourceActive(ID.ActiveSources, SourceID))
	{
		auto ActiveAIJob = std::find(ID.ActiveAISources.begin(), ID.ActiveAISources.end(), SourceID);
		if (ActiveAIJob != ID.ActiveAISources.end())
			ID.ActiveAISources.erase(ActiveAIJob);
	}
	ID.Condition.notify_all();
}

void ImageProcessingJobQueue::SetMaximumConcurrentAIJobs(size_t MaximumJobs)
{
	auto& ID = *m_InternalData;
	std::lock_guard<std::mutex> Lock(ID.QueueMutex);
	ID.MaximumConcurrentAIJobs = (std::max)(size_t{1}, MaximumJobs);
	ID.Condition.notify_all();
}

bool ImageProcessingJobQueue::HasLiveAIWork()
{
	auto& ID = *m_InternalData;
	std::lock_guard<std::mutex> Lock(ID.QueueMutex);
	return !ID.AIQueue.empty() || !ID.AIContinuationQueue.empty() || !ID.ActiveAISources.empty();
}

bool ImageProcessingJobQueue::IsCurrentJob(const SharedClassificationTask& Job)
{
	auto& ID = *m_InternalData;
	std::lock_guard<std::mutex> Lock(ID.QueueMutex);
	return IsCurrentGeneration(ID, Job);
}

bool ImageProcessingJobQueue::TryAcquireBackgroundAIJob()
{
	auto& ID = *m_InternalData;
	std::lock_guard<std::mutex> Lock(ID.QueueMutex);

	if (!ID.Queue.empty() || !ID.HighPriorityAsyncQueue.empty() ||
		!ID.AIQueue.empty() || !ID.AIContinuationQueue.empty() ||
		!ID.ActiveSources.empty() || !ID.ActiveAISources.empty() ||
		ID.ActiveAISources.size() + ID.ActiveBackgroundAIJobs >= ID.MaximumConcurrentAIJobs)
		return false;

	++ID.ActiveBackgroundAIJobs;
	return true;
}

void ImageProcessingJobQueue::CompletedBackgroundAIJob()
{
	auto& ID = *m_InternalData;
	std::lock_guard<std::mutex> Lock(ID.QueueMutex);
	if (ID.ActiveBackgroundAIJobs > 0)
		--ID.ActiveBackgroundAIJobs;
	ID.Condition.notify_all();
}

SourceStats ImageProcessingJobQueue::GetStats(int SourceID)
{
	return m_InternalData->GetStatsForSource(SourceID);
}

void ImageProcessingJobQueue::ResetStats(int SourceID)
{
	m_InternalData->ResetStats(SourceID);
}

void ImageProcessingJobQueue::CompletedJob(int SourceID, uint64_t Generation, bool ReleaseAISlot)
{
	auto& ID = *m_InternalData;
	std::unique_lock<std::mutex> Lock(ID.QueueMutex);

	auto ActiveJob = std::find(ID.ActiveSources.begin(), ID.ActiveSources.end(), SourceID);
	if (ActiveJob != ID.ActiveSources.end())
		ID.ActiveSources.erase(ActiveJob);

	if (ReleaseAISlot || Generation != CurrentGeneration(ID, SourceID))
	{
		auto ActiveAIJob = std::find(ID.ActiveAISources.begin(), ID.ActiveAISources.end(), SourceID);
		if (ActiveAIJob != ID.ActiveAISources.end())
			ID.ActiveAISources.erase(ActiveAIJob);
	}

	ID.Condition.notify_all();
}

void ImageProcessingJobQueue::WorkerThreadMain()
{
	auto& ID = *m_InternalData;
	ID.WantExit = false;
	SharedClassificationTask Job;
	Pop(Job);

	if (Job)
	{
		uint64_t TaskTimestamp = Job->Frame.Timestamp;
		int TaskSourceID = Job->Frame.SourceID;
		if (Job->Next)
		{
			auto StateInternal = ID.GetStateForSource(TaskSourceID);
			Job->Frame.WantFullSizeOutput = StateInternal->HasViewerFullSize;
			Job->Frame.WantSmallOutput = StateInternal->HasViewerPreviewSize;
			Job->Next->DoWork(Job);
			StateInternal->HasViewerFullSize = Job->Frame.WantFullSizeOutput;
			StateInternal->HasViewerPreviewSize = Job->Frame.WantSmallOutput;
		}
		else
		{
			Job->FrameOwner->InputFrame->Unref();
			ID.AddFrame(TaskSourceID, TaskTimestamp, Job->Frame.Stats);
		}

		// A reservation includes the observer callback, which can perform face
		// embedding. Release it only once the chain has reached its terminal job.
		const bool ReleaseAISlot = Job->HoldsAIReservation && Job->Next == nullptr;
		if (ReleaseAISlot)
			Job->HoldsAIReservation = false;
		CompletedJob(TaskSourceID, Job->QueueGeneration, ReleaseAISlot);
	}
}

void ImageProcessingJobQueue::RequestShutdown()
{
	auto& ID = *m_InternalData;
	ID.WantExit = true;
	MemoryBarrier();
	ID.Condition.notify_all();
}

void ImageProcessingJobQueueData::AddFrame(int Source, int64_t Timestamp, const FilterFrameStats& StatsIn)
{
	std::lock_guard<std::mutex> Lock(StatsMutex);
	auto& Ref = Stats[Source];
	Ref.FrameCount++;
	// Optional AI can finish after a newer essential frame has already been
	// accounted for, so a late completion must not move source progress back.
	Ref.LastTimestamp = (std::max)(Ref.LastTimestamp, Timestamp);
	for (unsigned int Stat = 0; Stat < FilterStat_Max; Stat++)
	{
		Ref.Stats.Stats[Stat] += StatsIn.Stats[Stat];
		Ref.Stats.FrameCount[Stat] += StatsIn.WasHit[Stat] ? 1 : 0;
	}
}

void ImageProcessingJobQueueData::ResetStats(int Source)
{
	std::lock_guard<std::mutex> Lock(StatsMutex);
	Stats[Source].Reset();
}

SourceState::SourceState()
	: FrameContext(std::make_shared<FilterFrameContext>())
	, HasViewerFullSize(false)
	, HasViewerPreviewSize(false)
{
}

SourceState::~SourceState()
{
	if (FrameContext && FrameContext->ConversionContext)
	{
		sws_freeContext(FrameContext->ConversionContext);
		FrameContext->ConversionContext = nullptr;
	}
}

}}
