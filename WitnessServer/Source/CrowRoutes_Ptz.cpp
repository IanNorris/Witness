#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "ReolinkClient.h"

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

		if (host && strlen(host) > 0 && user && pass)
		{
			client = ReolinkClient::AutoDetect(host, port > 0 ? port : 443, user, pass);
			if (client)
			{
				ctx.PtzClients[cameraId] = client;
				LOG_INFO("PTZ lazy-init succeeded for camera %d (%s)", cameraId, host);
			}
		}
		return true;
	});

	return client;
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

	if (!client)
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
		bool ok = client->PtzGoToPreset(presetId);
		crow::json::wvalue result;
		result["success"] = ok;
		if (!ok) result["error"] = client->GetLastError();
		res.code = ok ? 200 : 500;
		res.write(result.dump());
		res.end();
		return;
	}

	PtzOp op = ParsePtzCommand(command);
	bool ok = client->PtzControl(op, speed);

	crow::json::wvalue result;
	result["success"] = ok;
	if (!ok) result["error"] = client->GetLastError();
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
		res.code = 400;
		res.end();
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
		res.code = 400;
		res.end();
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
		res.code = 400;
		res.end();
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
		res.code = 400;
		res.end();
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
		res.code = 400;
		res.end();
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
		res.code = 400;
		res.end();
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
