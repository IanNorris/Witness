#pragma once

#include <string>
#include <mutex>
#include <chrono>

// Forward declare to avoid including heavy httplib.h in the header
namespace httplib { class Client; }

// PTZ operation commands supported by Reolink cameras
enum class PtzOp
{
	Left,
	Right,
	Up,
	Down,
	LeftUp,
	LeftDown,
	RightUp,
	RightDown,
	ZoomInc,
	ZoomDec,
	FocusInc,
	FocusDec,
	Stop,
	ToPos,      // Go to preset
};

struct PtzPosition
{
	int Pan = 0;    // Ppos (proprietary units)
	int Tilt = 0;   // Tpos (proprietary units)
	bool Valid = false;
};

struct PtzPreset
{
	int Id = 0;
	std::string Name;
	bool Enabled = false;
};

class ReolinkClient
{
public:
	ReolinkClient(const std::string& host, int port, bool useTls, const std::string& username, const std::string& password);
	~ReolinkClient();

	// Auto-detect protocol: tries HTTPS then HTTP on given port, then common ports
	static std::shared_ptr<ReolinkClient> AutoDetect(const std::string& host, int port, const std::string& username, const std::string& password);

	// Clear cached login failures for a host (call when credentials change)
	static void ClearFailureCache(const std::string& host);

	// Login and obtain a session token (auto-called on first API use)
	bool Login();

	// PTZ movement commands. Speed: 1-64 (default 32)
	bool PtzControl(PtzOp op, int speed = 32);
	bool PtzStop();
	bool PtzGoToPreset(int presetId);

	// Position query
	PtzPosition GetPosition();

	// Zoom/Focus absolute position control
	struct ZoomFocusState
	{
		int ZoomPos = 0;   // Current zoom position (device units, e.g. 0-33)
		int ZoomMin = 0;   // Min zoom position (from device range)
		int ZoomMax = 0;   // Max zoom position (from device range, e.g. 33)
		int FocusPos = 0;  // Current focus position
		int FocusMin = 0;  // Min focus position
		int FocusMax = 0;  // Max focus position (e.g. 223)
		bool Valid = false;
	};
	ZoomFocusState GetZoomFocus();
	bool SetZoomPos(int zoomPos);
	bool SetFocusPos(int focusPos);

	// Preset management
	std::vector<PtzPreset> GetPresets();
	bool SetPreset(int id, const std::string& name);
	bool DeletePreset(int id);

	// AI/motion detection state query
	bool GetMotionState(int channel = 0);

	// Reboot the camera (used as last resort when RTSP server is stuck)
	bool Reboot();

	// Connection test
	bool IsReachable();

	// Get last error message
	const std::string& GetLastError() const { return m_LastError; }

private:
	// Send a command to the camera API, returns response body or empty on failure
	std::string SendCommand(const std::string& cmd, const std::string& body);

	// Ensure we have a valid token (login if needed)
	bool EnsureToken();

	static const char* PtzOpToString(PtzOp op);

	std::string m_Host;
	int m_Port;
	std::string m_Username;
	std::string m_Password;
	std::string m_Token;
	std::string m_LastError;

	std::chrono::steady_clock::time_point m_TokenExpiry;

	std::mutex m_Mutex;
	std::unique_ptr<httplib::Client> m_HttpClient;
};
