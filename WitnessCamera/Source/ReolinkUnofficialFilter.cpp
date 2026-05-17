// ReolinkUnofficialFilter — Motion filter using Reolink Baichuan AI detections
//
// DISCLAIMER: This is an UNOFFICIAL implementation based on community-researched
// protocol details. It is not endorsed by or affiliated with Reolink Technology Co., Ltd.
// Use at your own risk.

#include "ReolinkUnofficialFilter.h"
#include "RecordFilter.h"
#include <Log.h>

namespace Witness {
namespace Camera {

struct ReolinkUnofficialFilterData
{
	// Detection staleness threshold — if no fresh detection data arrives within
	// this window, we consider the camera has no active detections.
	static constexpr int DETECTION_STALE_MS = 2000;
};

PIMPL_CONSTRUCT(ReolinkUnofficialFilterData)

ReolinkUnofficialFilter::ReolinkUnofficialFilter(
	const MotionChainNode& Chain,
	const std::string& host,
	int port,
	const std::string& username,
	const std::string& password)
	: RecordFilterBase<ReolinkUnofficialFilterData>(Chain)
{
	m_Client = std::make_shared<ReolinkBaichuanClient>(host, port, username, password);
	m_Client->Start();

	LOG_INFO("ReolinkUnofficialFilter: connecting to %s:%d (user=%s)", host.c_str(), port, username.c_str());
}

ReolinkUnofficialFilter::~ReolinkUnofficialFilter()
{
	if (m_Client)
	{
		m_Client->Stop();
	}
}

bool ReolinkUnofficialFilter::ProcessFrame(SharedClassificationTask TaskData)
{
	auto state = m_Client->GetDetections();

	if (!state.HasData)
	{
		// No detection data available — treat as no motion
		TaskData->Result.MotionAmount = 0.0f;
		return true;
	}

	// Check if detection data is stale
	auto now = std::chrono::steady_clock::now();
	auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.Timestamp).count();
	if (ageMs > ReolinkUnofficialFilterData::DETECTION_STALE_MS)
	{
		TaskData->Result.MotionAmount = 0.0f;
		return true;
	}

	if (state.Detections.empty())
	{
		TaskData->Result.MotionAmount = 0.0f;
		return true;
	}

	// We have active detections — set motion flags and ROI
	TaskData->Result.MotionAmount = 1.0f;
	TaskData->Result.ClassificationSuperset |= ClassificationResult::Motion_Motion;

	unsigned int frameWidth = TaskData->Frame.InputFrame->GetWidth();
	unsigned int frameHeight = TaskData->Frame.InputFrame->GetHeight();

	for (const auto& det : state.Detections)
	{
		ClassificationResult::RegionOfInterest roi;
		roi.Filter = this;
		roi.Left = (unsigned int)(det.X1 * frameWidth);
		roi.Top = (unsigned int)(det.Y1 * frameHeight);
		roi.Width = (unsigned int)((det.X2 - det.X1) * frameWidth);
		roi.Height = (unsigned int)((det.Y2 - det.Y1) * frameHeight);
		roi.ClassificationConfidence = det.Confidence;

		switch (det.DetectionClass)
		{
		case ReolinkDetection::People:
			roi.Classification = ClassificationResult::Motion_Person;
			roi.ClassificationGroup = ClassificationResult::Motion_Person;
			TaskData->Result.ClassificationSuperset |= ClassificationResult::Motion_Person;
			break;
		case ReolinkDetection::Vehicle:
			roi.Classification = ClassificationResult::Motion_Vehicle;
			roi.ClassificationGroup = ClassificationResult::Motion_Vehicle;
			TaskData->Result.ClassificationSuperset |= ClassificationResult::Motion_Vehicle;
			break;
		case ReolinkDetection::Animal:
			roi.Classification = ClassificationResult::Motion_Animal;
			roi.ClassificationGroup = ClassificationResult::Motion_Animal;
			TaskData->Result.ClassificationSuperset |= ClassificationResult::Motion_Animal;
			break;
		case ReolinkDetection::Face:
			roi.Classification = ClassificationResult::Motion_Face;
			roi.ClassificationGroup = ClassificationResult::Motion_Face;
			TaskData->Result.ClassificationSuperset |= ClassificationResult::Motion_Face;
			break;
		}

		TaskData->Result.ROI.push_back(roi);
	}

	return true;
}

void ReolinkUnofficialFilter::ClearStateThis()
{
	// Nothing to clear — detection state is managed by the client
}

bool ReolinkUnofficialFilter::IsConnected() const
{
	return m_Client && m_Client->IsConnected();
}

}}
