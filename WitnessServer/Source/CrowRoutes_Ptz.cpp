#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "ReolinkClient.h"

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
	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto client = m_GlobalContext->GetPtzClient(cameraId);
	if (!client)
	{
		// Check if there's a linked camera with PTZ
		SQLiteDatabaseQueryInstance GetLinked(m_GlobalContext->Database, "GetLinkedCameraId");
		GetLinked->Bind("@CameraId", cameraId);
		GetLinked->Execute([&](const SQLiteDatabaseQuery& query)
		{
			int linkedId = query.GetColumnValueInt(0);
			client = m_GlobalContext->GetPtzClient(linkedId);
			return true;
		});
	}

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
	if (!req.body.empty())
	{
		auto body = crow::json::load(req.body);
		if (body && body.has("speed"))
			speed = (int)body["speed"].i();
	}

	// Handle preset command specially
	if (command == "preset")
	{
		auto body = crow::json::load(req.body);
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

	auto client = m_GlobalContext->GetPtzClient(cameraId);
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

	auto client = m_GlobalContext->GetPtzClient(cameraId);
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
	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto body = crow::json::load(req.body);
	if (!body || !body.has("id") || !body.has("name"))
	{
		res.code = 400;
		res.end();
		return;
	}

	auto client = m_GlobalContext->GetPtzClient(cameraId);
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
	int UserUID = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator, cameraId);
	if (UserUID <= 0)
	{
		res.code = 403;
		res.end();
		return;
	}

	auto body = crow::json::load(req.body);
	if (!body || !body.has("id"))
	{
		res.code = 400;
		res.end();
		return;
	}

	auto client = m_GlobalContext->GetPtzClient(cameraId);
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
