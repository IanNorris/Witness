#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "ReolinkClient.h"
#include "ReolinkBaichuanClient.h"
#include "SQLite.h"
#include "UrlHelpers.h"

// Try to lazily initialize a PTZ client if it wasn't created at startup
static std::shared_ptr<ReolinkClient> TryInitPtzClient(GlobalContext& ctx, int cameraId)
{
	SQLiteDatabaseQueryInstance GetPtzConfig(ctx.Database, "GetCameraPtzConfig");
	GetPtzConfig->Bind("@CameraId", cameraId);

	std::shared_ptr<ReolinkClient> client;
	GetPtzConfig->Execute([&](const SQLiteDatabaseQuery& query)
	{
		int ptzEnabled = query.GetColumnValueInt(0);
		if (!ptzEnabled) return true;

		const char* host = query.GetColumnValueText(1);
		int port = query.GetColumnValueInt(2);
		const char* user = query.GetColumnValueText(3);
		const char* pass = query.GetColumnValueText(4);
		const char* rtspPath = query.GetColumnValueText(5);

		std::string hostStr = (host && strlen(host) > 0) ? host : "";
		std::string userStr = (user && strlen(user) > 0) ? user : "";
		std::string passStr = (pass && strlen(pass) > 0) ? pass : "";

		// Fall back to RTSP URL credentials if PTZ-specific ones are empty
		// Note: we only take host/user/pass from RTSP URL, NOT port (RTSP uses 554, API uses 443/80)
		if (rtspPath && strlen(rtspPath) > 0 && (userStr.empty() || passStr.empty() || hostStr.empty()))
		{
			std::string rtspUser, rtspPass, rtspHost;
			int rtspPort = 0;
			ParseRtspUrl(rtspPath, rtspUser, rtspPass, rtspHost, rtspPort);

			LOG_INFO("PTZ lazy-init: falling back to RTSP URL creds for camera %d (host=%s, user=%s, pass=%s)",
				cameraId, rtspHost.c_str(), rtspUser.c_str(), rtspPass.empty() ? "<empty>" : "<present>");

			if (userStr.empty()) userStr = rtspUser;
			if (passStr.empty()) passStr = rtspPass;
			if (hostStr.empty()) hostStr = rtspHost;
		}

		if (!hostStr.empty() && !userStr.empty() && !passStr.empty())
		{
			client = ReolinkClient::AutoDetect(hostStr, port > 0 ? port : 443, userStr, passStr);
			if (client)
			{
				ctx.PtzClients[cameraId] = client;
				LOG_INFO("PTZ lazy-init succeeded for camera %d (%s)", cameraId, hostStr.c_str());
			}
			else
			{
				LOG_ERROR("PTZ lazy-init failed for camera %d (%s:%d, user=%s)", cameraId, hostStr.c_str(), port > 0 ? port : 443, userStr.c_str());
			}
		}
		else
		{
			LOG_ERROR("PTZ lazy-init: insufficient credentials for camera %d (host=%s, user=%s, pass=%s)",
				cameraId, hostStr.empty() ? "<empty>" : hostStr.c_str(),
				userStr.empty() ? "<empty>" : userStr.c_str(),
				passStr.empty() ? "<empty>" : "<present>");
		}
		return true;
	});

	return client;
}

static void RespondNoPtz(crow::response& res)
{
	crow::json::wvalue err;
	err["error"] = "Camera does not have PTZ enabled";
	res.code = 400;
	res.write(err.dump());
	res.end();
}

// Get PTZ client, trying linked camera and lazy init
static std::shared_ptr<ReolinkClient> GetPtzClientForCamera(GlobalContext& ctx, int cameraId)
{
	auto client = ctx.GetPtzClient(cameraId);
	if (client) return client;

	// Check linked camera
	SQLiteDatabaseQueryInstance GetLinked(ctx.Database, "GetLinkedCameraId");
	GetLinked->Bind("@CameraId", cameraId);
	int linkedId = 0;
	GetLinked->Execute([&](const SQLiteDatabaseQuery& query)
	{
		linkedId = query.GetColumnValueInt(0);
		return true;
	});

	if (linkedId > 0)
	{
		client = ctx.GetPtzClient(linkedId);
		if (client) return client;
		// Try lazy init on linked camera
		client = TryInitPtzClient(ctx, linkedId);
		if (client) return client;
	}

	// Try lazy init on the camera itself
	return TryInitPtzClient(ctx, cameraId);
}

static PtzOp ParsePtzCommand(const std::string& cmd)
{
	if (cmd == "left")      return PtzOp::Left;
	if (cmd == "right")     return PtzOp::Right;
	if (cmd == "up")        return PtzOp::Up;
	if (cmd == "down")      return PtzOp::Down;
	if (cmd == "leftup")    return PtzOp::LeftUp;
	if (cmd == "leftdown")  return PtzOp::LeftDown;
	if (cmd == "rightup")   return PtzOp::RightUp;
	if (cmd == "rightdown") return PtzOp::RightDown;
	if (cmd == "zoomin")    return PtzOp::ZoomInc;
	if (cmd == "zoomout")   return PtzOp::ZoomDec;
	if (cmd == "focusin")   return PtzOp::FocusInc;
	if (cmd == "focusout")  return PtzOp::FocusDec;
	if (cmd == "stop")      return PtzOp::Stop;
	return PtzOp::Stop;
}

static const char* PtzOpToBaichuanCommand(PtzOp op)
{
	switch (op)
	{
		case PtzOp::Left:      return "Left";
		case PtzOp::Right:     return "Right";
		case PtzOp::Up:        return "Up";
		case PtzOp::Down:      return "Down";
		case PtzOp::LeftUp:    return "LeftUp";
		case PtzOp::LeftDown:  return "LeftDown";
		case PtzOp::RightUp:   return "RightUp";
		case PtzOp::RightDown: return "RightDown";
		case PtzOp::ZoomInc:   return "ZoomInc";
		case PtzOp::ZoomDec:   return "ZoomDec";
		case PtzOp::FocusInc:  return "FocusInc";
		case PtzOp::FocusDec:  return "FocusDec";
		case PtzOp::Stop:      return "Stop";
		default:               return "Stop";
	}
}

// Try PTZ command via Baichuan protocol (returns true if handled)
static bool TryBaichuanPtz(std::shared_ptr<Witness::Camera::ReolinkBaichuanClient> bcClient, PtzOp op, int speed, int channel = 0)
{
	if (!bcClient || !bcClient->IsConnected())
		return false;

	if (op == PtzOp::Stop)
		return bcClient->PtzStop(channel);

	// Zoom/focus commands work better without speed on Reolink cameras
	int bcSpeed = speed;
	if (op == PtzOp::ZoomInc || op == PtzOp::ZoomDec || op == PtzOp::FocusInc || op == PtzOp::FocusDec)
		bcSpeed = 0;

	return bcClient->PtzControl(PtzOpToBaichuanCommand(op), bcSpeed, channel);
}

static bool TryBaichuanPreset(std::shared_ptr<Witness::Camera::ReolinkBaichuanClient> bcClient, int presetId, int channel = 0)
{
	if (!bcClient || !bcClient->IsConnected())
		return false;

	return bcClient->PtzGoToPreset(presetId, channel);
}

void CrowListener::HandlePtzCommand(const crow::request& req, crow::response& res, int cameraId, const std::string& command)
{
	auto body = crow::json::load(req.body);

	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, body ? &body : nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);

	// Try Baichuan client: direct camera first, then linked camera
	auto bcClient = m_GlobalContext->GetBaichuanClient(cameraId);
	if (!bcClient)
	{
		SQLiteDatabaseQueryInstance GetLinked(m_GlobalContext->Database, "GetLinkedCameraId");
		GetLinked->Bind("@CameraId", cameraId);
		int linkedId = 0;
		GetLinked->Execute([&](const SQLiteDatabaseQuery& query)
		{
			linkedId = query.GetColumnValueInt(0);
			return true;
		});
		if (linkedId > 0)
			bcClient = m_GlobalContext->GetBaichuanClient(linkedId);
	}

	if (!client && !bcClient)
	{
		crow::json::wvalue err;
		err["error"] = "Camera does not have PTZ enabled";
		res.code = 400;
		res.write(err.dump());
		res.end();
		return;
	}

	// Parse speed from body if present
	int speed = 32;
	if (body && body.has("speed"))
		speed = (int)body["speed"].i();

	// Handle preset command specially
	if (command == "preset")
	{
		if (!body || !body.has("id"))
		{
			res.code = 400;
			crow::json::wvalue err;
			err["error"] = "preset command requires 'id' in body";
			res.write(err.dump());
			res.end();
			return;
		}
		int presetId = (int)body["id"].i();

		// Try Baichuan first (more reliable for Reolink cameras)
		bool ok = TryBaichuanPreset(bcClient, presetId);
		if (!ok && client)
			ok = client->PtzGoToPreset(presetId);

		crow::json::wvalue result;
		result["success"] = ok;
		if (!ok) result["error"] = client ? client->GetLastError() : "No PTZ connection available";
		res.code = ok ? 200 : 500;
		res.write(result.dump());
		res.end();
		return;
	}

	PtzOp op = ParsePtzCommand(command);

	// Try Baichuan first (more reliable for Reolink cameras)
	bool ok = TryBaichuanPtz(bcClient, op, speed);
	if (!ok && client)
		ok = client->PtzControl(op, speed);

	crow::json::wvalue result;
	result["success"] = ok;
	if (!ok) result["error"] = client ? client->GetLastError() : "No PTZ connection available";
	res.code = ok ? 200 : 500;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzPosition(const crow::request& req, crow::response& res, int cameraId)
{
	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	PtzPosition pos = client->GetPosition();
	crow::json::wvalue result;
	result["valid"] = pos.Valid;
	result["pan"] = pos.Pan;
	result["tilt"] = pos.Tilt;
	res.code = 200;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzZoomGet(const crow::request& req, crow::response& res, int cameraId)
{
	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	auto state = client->GetZoomFocus();
	crow::json::wvalue result;
	result["valid"] = state.Valid;
	result["zoom"] = state.ZoomPos;
	result["zoomMin"] = state.ZoomMin;
	result["zoomMax"] = state.ZoomMax;
	result["focus"] = state.FocusPos;
	result["focusMin"] = state.FocusMin;
	result["focusMax"] = state.FocusMax;
	res.code = 200;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzZoomSet(const crow::request& req, crow::response& res, int cameraId)
{
	auto body = crow::json::load(req.body);

	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, body ? &body : nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	if (!body || !body.has("zoom"))
	{
		res.code = 400;
		crow::json::wvalue err;
		err["error"] = "Missing 'zoom' parameter";
		res.write(err.dump());
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	int zoomPos = (int)body["zoom"].i();
	bool ok = client->SetZoomPos(zoomPos);

	crow::json::wvalue result;
	result["success"] = ok;
	if (!ok) result["error"] = client->GetLastError();
	res.code = ok ? 200 : 500;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzPresets(const crow::request& req, crow::response& res, int cameraId)
{
	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	auto presets = client->GetPresets();
	std::vector<crow::json::wvalue> presetsArray;
	for (auto& p : presets)
	{
		crow::json::wvalue preset;
		preset["id"] = p.Id;
		preset["name"] = p.Name;
		preset["enabled"] = p.Enabled;
		presetsArray.push_back(std::move(preset));
	}

	crow::json::wvalue result;
	result["presets"] = std::move(presetsArray);
	res.code = 200;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzPresetSet(const crow::request& req, crow::response& res, int cameraId)
{
	auto body = crow::json::load(req.body);

	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, body ? &body : nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	if (!body || !body.has("id") || !body.has("name"))
	{
		res.code = 400;
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	int id = (int)body["id"].i();
	std::string name = body["name"].s();
	bool ok = client->SetPreset(id, name);

	crow::json::wvalue result;
	result["success"] = ok;
	res.code = ok ? 200 : 500;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzPresetDelete(const crow::request& req, crow::response& res, int cameraId)
{
	auto body = crow::json::load(req.body);

	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, body ? &body : nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	if (!body || !body.has("id"))
	{
		res.code = 400;
		res.end();
		return;
	}

	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	int id = (int)body["id"].i();
	bool ok = client->DeletePreset(id);

	crow::json::wvalue result;
	result["success"] = ok;
	res.code = ok ? 200 : 500;
	res.write(result.dump());
	res.end();
}

void CrowListener::HandlePtzTest(const crow::request& req, crow::response& res)
{
	auto body = crow::json::load(req.body);

	int UserUID = CrowAuth::IsAuthenticated(*m_GlobalContext, req, body ? &body : nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	if (!body || !body.has("host") || !body.has("username") || !body.has("password"))
	{
		crow::json::wvalue err;
		err["error"] = "Missing required fields: host, username, password";
		res.code = 400;
		res.write(err.dump());
		res.end();
		return;
	}

	std::string host = std::string(body["host"].s());
	int port = body.has("port") ? (int)body["port"].i() : 443;
	std::string username = std::string(body["username"].s());
	std::string password = std::string(body["password"].s());

	// Clear failure cache for this host so the test always actually tries
	ReolinkClient::ClearFailureCache(host);

	auto client = ReolinkClient::AutoDetect(host, port > 0 ? port : 443, username, password);

	crow::json::wvalue result;
	if (client)
	{
		result["success"] = true;
		result["message"] = "Connected successfully";

		// Try getting position to verify full functionality
		auto pos = client->GetPosition();
		result["positionValid"] = pos.Valid;
	}
	else
	{
		result["success"] = false;
		result["message"] = "Connection failed — check host, port, username, and password";
	}

	res.code = 200;
	res.write(result.dump());
	res.end();
}
