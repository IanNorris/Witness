#pragma once

#include "Export.h"

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace Witness {
namespace Camera {

// Detection box from Reolink AI
struct ReolinkDetection
{
	enum Class : uint8_t
	{
		People = 1,
		Vehicle = 2,
		Animal = 3,
		Face = 4
	};

	Class DetectionClass;
	float X1, Y1, X2, Y2; // Normalized [0,1] coordinates
	float Confidence;      // 0-1
};

// Current detection state (thread-safe snapshot)
struct ReolinkDetectionState
{
	std::vector<ReolinkDetection> Detections;
	std::chrono::steady_clock::time_point Timestamp;
	bool HasData = false;
	bool HasMotion = false;
};

// Baichuan protocol client for Reolink cameras (port 9000)
// Connects to camera, authenticates, and continuously receives AI detection data.
// This is an UNOFFICIAL implementation based on community-researched protocol details.
class CAMERA_API ReolinkBaichuanClient
{
public:
	ReolinkBaichuanClient(const std::string& host, int port, const std::string& username, const std::string& password);
	~ReolinkBaichuanClient();

	// Stable-address thread ownership; never copy or move an active client.
	ReolinkBaichuanClient(const ReolinkBaichuanClient&) = delete;
	ReolinkBaichuanClient& operator=(const ReolinkBaichuanClient&) = delete;
	ReolinkBaichuanClient(ReolinkBaichuanClient&&) = delete;
	ReolinkBaichuanClient& operator=(ReolinkBaichuanClient&&) = delete;

	// Start/stop the background connection thread
	void Start();
	void Stop();

	// Get current detection state (thread-safe)
	ReolinkDetectionState GetDetections() const;

	// PTZ commands (sent over existing Baichuan connection)
	bool PtzControl(const std::string& command, int speed = 32, int channel = 0);
	bool PtzGoToPreset(int presetId, int channel = 0);
	bool PtzStop(int channel = 0);

	// Connection status
	bool IsConnected() const;
	std::string GetLastError() const;

private:
	class Impl;
	Impl* m_Impl;
};

}}
