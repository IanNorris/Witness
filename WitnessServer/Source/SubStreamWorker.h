#pragma once

#include <InputStream.h>
#include <LiveOutputStream.h>
#include <thread>
#include <atomic>
#include <string>
#include <memory>

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

	std::shared_ptr<Witness::Camera::LiveOutputStream>& GetLiveStream() { return m_LiveStream; }
	std::string GetCodecName() const;

	bool IsConnected() const { return m_Connected.load(); }

private:
	void ThreadFunc();

	int m_CameraId;
	std::string m_SubStreamUrl;
	std::string m_CachePath;
	std::shared_ptr<GlobalContext> m_Context;

	std::shared_ptr<Witness::Camera::InputStream> m_InputStream;
	std::shared_ptr<Witness::Camera::LiveOutputStream> m_LiveStream;

	std::thread m_Thread;
	std::atomic<bool> m_Running{false};
	std::atomic<bool> m_Connected{false};
};
