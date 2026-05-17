#pragma once

#include "Export.h"

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
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
};

// Baichuan protocol client for Reolink cameras (port 9000)
// Connects to camera, authenticates, and continuously receives AI detection data.
// This is an UNOFFICIAL implementation based on community-researched protocol details.
class CAMERA_API ReolinkBaichuanClient
{
public:
	ReolinkBaichuanClient(const std::string& host, int port, const std::string& username, const std::string& password);
	~ReolinkBaichuanClient();

	// Start/stop the background connection thread
	void Start();
	void Stop();

	// Get current detection state (thread-safe)
	ReolinkDetectionState GetDetections() const;

	// Connection status
	bool IsConnected() const { return m_Connected.load(); }
	std::string GetLastError() const;

private:
	void ThreadFunc();
	bool Connect();
	bool Authenticate();
	bool RequestStream();
	void ReadLoop();
	bool ReadExact(uint8_t* buffer, size_t length);
	bool SendRaw(const uint8_t* data, size_t length);
	bool SendBcMessage(uint32_t cmdId, uint32_t msgId, const std::vector<uint8_t>& payload);
	bool ParseBcMediaFrame(const std::vector<uint8_t>& frame);
	void ParseBcMediaFrames(const uint8_t* data, size_t length);
	void ParseDetectionData(const uint8_t* data, size_t length, bool isIFrame);
	void ParseOuterTLV(const uint8_t* data, size_t length);
	void DecompressAndParseTLV(const uint8_t* data, size_t length);
	void ParseInnerDetectionTLV(const uint8_t* data, size_t length);
	void ParseTLVRecursive(const uint8_t* data, size_t length,
		uint8_t contextType1, uint8_t contextType2, std::vector<ReolinkDetection>& detections);

	std::string m_Host;
	int m_Port;
	std::string m_Username;
	std::string m_Password;

	// Socket
	uintptr_t m_Socket = ~(uintptr_t)0; // INVALID_SOCKET

	// Thread
	std::thread m_Thread;
	std::atomic<bool> m_Running{false};
	std::atomic<bool> m_Connected{false};

	// Detection state (protected by mutex)
	mutable std::mutex m_DetectionMutex;
	ReolinkDetectionState m_CurrentDetections;

	// Error state
	mutable std::mutex m_ErrorMutex;
	std::string m_LastError;

	// Auth token from login
	std::string m_AuthToken;
	uint32_t m_MessageCounter = 0;

	// AI coordinate space (from camera, typically 896x480)
	uint16_t m_AiWidth = 896;
	uint16_t m_AiHeight = 480;

	// TLS state (for cameras with Baichuan over TLS)
	void* m_SslCtx = nullptr;  // SSL_CTX*
	void* m_Ssl = nullptr;     // SSL*
	bool m_UseTls = false;

	void CleanupTls();
	bool TryTlsConnect();
};

}}
