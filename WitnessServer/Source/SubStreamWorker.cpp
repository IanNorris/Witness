#include "SubStreamWorker.h"
#include "GlobalContext.h"
#include "Common.h"
#include "UrlHelpers.h"
#include <Log.h>
#include <chrono>
#include <format>

using namespace Witness::Camera;

SubStreamWorker::SubStreamWorker(int cameraId, const std::string& subStreamUrl, const std::string& cachePath, const std::shared_ptr<GlobalContext>& context)
	: m_CameraId(cameraId)
	, m_SubStreamUrl(subStreamUrl)
	, m_CachePath(cachePath)
	, m_Context(context)
{
}

SubStreamWorker::~SubStreamWorker()
{
	Stop();
}

void SubStreamWorker::Start()
{
	if (m_Running.load())
		return;

	m_Running = true;
	m_Thread = std::thread(&SubStreamWorker::ThreadFunc, this);
}

void SubStreamWorker::Stop()
{
	m_Running = false;
	if (m_Thread.joinable())
		m_Thread.join();
}

std::string SubStreamWorker::GetCodecName() const
{
	std::lock_guard<std::mutex> lock(m_StreamMetadataMutex);
	return m_VideoCodecName;
}

int SubStreamWorker::GetVideoWidth() const
{
	return m_VideoWidth.load();
}

int SubStreamWorker::GetVideoHeight() const
{
	return m_VideoHeight.load();
}

void SubStreamWorker::ThreadFunc()
{
	LOG_INFO("[SubStream] Camera %d sub-stream worker starting: %s", m_CameraId, m_SubStreamUrl.c_str());

	while (m_Running.load())
	{
		// Create input stream in passthrough mode (no decoding)
		InputStreamSetup setup;
		setup.GetTimestamp = GetUnixTimestamp;
		setup.MotionFilterFrameSkip = 1;
		setup.MotionDetectFrameHeight = 720;
		setup.MotionDetectThreshold = 0.1;
		setup.HistoricalPacketBufferSeconds = 0; // No backlog needed for sub-stream
		setup.ExportMotionVectors = false;
		setup.PassthroughOnly = true;

		m_InputStream = std::make_shared<InputStream>(setup, m_CameraId, nullptr, m_SubStreamUrl);
		{
			std::lock_guard<std::mutex> lock(m_StreamMetadataMutex);
			m_VideoCodecName = m_InputStream->GetCodecName();
		}
		m_VideoWidth = m_InputStream->GetVideoWidth();
		m_VideoHeight = m_InputStream->GetVideoHeight();

		if (!m_LiveStream)
		{
			m_LiveStream = std::make_shared<LiveOutputStream>(m_CachePath, m_InputStream.get(), 1);
			m_LiveStream->SetTimestampNormalizationAllowed(
				DetectCameraProfile(m_SubStreamUrl) == CameraProfile::Reolink);

			// Wire up MSE WebSocket notifications for sub-stream channel
			int cameraId = m_CameraId;
			auto streams = m_Context->Streams;
			int subChannelId = cameraId + StreamBroadcaster::SubStreamChannelOffset;

			m_LiveStream->SetEventCallback([cameraId, subChannelId, streams](const LiveStreamEvent& ev)
			{
				if (!streams->HasViewers(subChannelId))
					return;

				crow::json::wvalue ctrl;
				switch (ev.EventType)
				{
				case LiveStreamEvent::InitSegmentReady:
					ctrl["type"] = "initSegment";
					ctrl["generation"] = ev.Generation;
					if (!ev.AudioCodec.empty())
						ctrl["audioCodec"] = ev.AudioCodec;
					ctrl["bytes"] = (int64_t)ev.ByteSize;
					ctrl["hash"] = std::format("{:08x}", ev.TransportHash);
					streams->SendControl(subChannelId, ctrl.dump());
					streams->SendBinary(subChannelId, ev.Data);
					break;

				case LiveStreamEvent::PartialReady:
					ctrl["type"] = "partial";
					ctrl["generation"] = ev.Generation;
					ctrl["segmentIndex"] = ev.SegmentIndex;
					ctrl["partIndex"] = ev.PartIndex;
					ctrl["duration"] = ev.Duration;
					ctrl["independent"] = ev.Independent;
					ctrl["keyframeSeekSafe"] = ev.KeyframeSeekSafe;
					ctrl["bytes"] = (int64_t)ev.ByteSize;
					ctrl["hash"] = std::format("{:08x}", ev.TransportHash);
					streams->SendControl(subChannelId, ctrl.dump());
					streams->SendBinary(subChannelId, ev.Data);
					break;

				case LiveStreamEvent::SegmentReady:
					ctrl["type"] = "segment";
					ctrl["generation"] = ev.Generation;
					ctrl["segmentIndex"] = ev.SegmentIndex;
					ctrl["duration"] = ev.Duration;
					streams->SendControl(subChannelId, ctrl.dump());
					break;

				case LiveStreamEvent::Discontinuity:
					ctrl["type"] = "discontinuity";
					ctrl["generation"] = ev.Generation;
					streams->SendControl(subChannelId, ctrl.dump());
					break;

				case LiveStreamEvent::DecodeCorruption:
					ctrl["type"] = "decodeCorruption";
					ctrl["generation"] = ev.Generation;
					ctrl["segmentIndex"] = ev.SegmentIndex;
					ctrl["partIndex"] = ev.PartIndex;
					ctrl["decodeErrorFlags"] = ev.DecodeErrorFlags;
					streams->SendControl(subChannelId, ctrl.dump());
					break;

				case LiveStreamEvent::DecodeRecovery:
					ctrl["type"] = "decodeRecovery";
					ctrl["generation"] = ev.Generation;
					ctrl["segmentIndex"] = ev.SegmentIndex;
					ctrl["partIndex"] = ev.PartIndex;
					streams->SendControl(subChannelId, ctrl.dump());
					break;
				}
			});
		}
		else
		{
			m_LiveStream->ResetForReconnect(m_InputStream.get());
		}
		{
			std::lock_guard<std::mutex> lock(m_StreamMetadataMutex);
			m_PublishedLiveStream = m_LiveStream;
		}

		m_Connected = true;
		m_ReconnectBackoff = 5000; // Reset backoff on successful connection
		LOG_INFO("[SubStream] Camera %d sub-stream connected", m_CameraId);

		// Process packets in a loop
		while (m_Running.load())
		{
			CameraStreamError error = m_InputStream->ProcessFrame(nullptr, nullptr, m_LiveStream.get());

			if (error != CameraStreamError::Success)
			{
				m_Connected = false;
				std::string errorMsg = GetCameraStreamErrorMessage(error);
				if (m_InputStream->GetFFMPEGErrorMessage()[0] != '\0')
				{
					errorMsg += ": ";
					errorMsg += m_InputStream->GetFFMPEGErrorMessage();
				}
				LOG_WARNING("[SubStream] Camera %d sub-stream error: %s", m_CameraId, errorMsg.c_str());

				// Wait before reconnecting (with exponential backoff for auth failures)
				if (error != CameraStreamError::EndOfFile)
				{
					bool isAuthError = (errorMsg.find("401") != std::string::npos ||
						errorMsg.find("Unauthorized") != std::string::npos ||
						errorMsg.find("authorization") != std::string::npos);

					if (isAuthError)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(m_ReconnectBackoff));
						m_ReconnectBackoff = std::min(m_ReconnectBackoff * 2, 60000);
					}
					else
					{
						std::this_thread::sleep_for(std::chrono::seconds(5));
					}
				}
				break; // Break inner loop to reconnect
			}
			if (m_VideoWidth.load() == 0 || m_VideoHeight.load() == 0)
			{
				std::lock_guard<std::mutex> lock(m_StreamMetadataMutex);
				m_VideoCodecName = m_InputStream->GetCodecName();
				m_VideoWidth = m_InputStream->GetVideoWidth();
				m_VideoHeight = m_InputStream->GetVideoHeight();
			}
		}
	}

	m_Connected = false;
	{
		std::lock_guard<std::mutex> lock(m_StreamMetadataMutex);
		m_VideoCodecName.clear();
	}
	m_VideoWidth = 0;
	m_VideoHeight = 0;
	LOG_INFO("[SubStream] Camera %d sub-stream worker stopped", m_CameraId);
}
