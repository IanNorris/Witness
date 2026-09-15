#pragma once

#include <InputStream.h>
#include <LiveOutputStream.h>
#include <thread>
#include <atomic>
#include <string>
#include <memory>
#include <mutex>

class GlobalContext;

// Lightweight worker that connects to a camera's sub-stream (typically H.264)
// and generates HLS segments. No motion detection, no filters, no recording.
// Runs in its own thread. Used as a fallback for clients that can't decode
// the main stream codec (e.g. HEVC without browser support).
class SubStreamWorker
{
public:
	SubStreamWorker(int cameraId, const std::string& subStreamUrl, const std::string& cachePath, const std::shared_ptr<GlobalContext>& context);
	~SubStreamWorker();

	void Start();
	void Stop();

	std::shared_ptr<Witness::Camera::LiveOutputStream> GetLiveStream() const
	{
		std::lock_guard<std::mutex> lock(m_StreamMetadataMutex);
		return m_PublishedLiveStream;
	}
	std::string GetCodecName() const;
	int GetVideoWidth() const;
	int GetVideoHeight() const;

	bool IsConnected() const { return m_Connected.load(); }

private:
	void ThreadFunc();

	int m_CameraId;
	std::string m_SubStreamUrl;
	std::string m_CachePath;
	std::shared_ptr<GlobalContext> m_Context;

	std::shared_ptr<Witness::Camera::InputStream> m_InputStream;
	std::shared_ptr<Witness::Camera::LiveOutputStream> m_LiveStream;
	std::shared_ptr<Witness::Camera::LiveOutputStream> m_PublishedLiveStream;
	mutable std::mutex m_StreamMetadataMutex;
	std::string m_VideoCodecName;
	std::atomic<int> m_VideoWidth{0};
	std::atomic<int> m_VideoHeight{0};

	std::thread m_Thread;
	std::atomic<bool> m_Running{false};
	std::atomic<bool> m_Connected{false};
	int m_ReconnectBackoff{5000}; // milliseconds, grows for auth failures
};
