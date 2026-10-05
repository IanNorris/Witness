#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "ReolinkClient.h"
#include "ReolinkBaichuanClient.h"
#include "SQLite.h"
#include "UrlHelpers.h"
#include "PtzMovementLease.h"
#include "PtzConfiguration.h"

static int PtzLaneId(GlobalContext& ctx, int cameraId)
{
    return ReadPtzLaneId(ctx.Database, cameraId);
}

// Try to lazily initialize a PTZ client if it wasn't created at startup
static std::shared_ptr<ReolinkClient> TryInitPtzClient(GlobalContext& ctx, int cameraId)
{
    auto Config = ReadPtzConfiguration(ctx.Database, cameraId);
    if (!Config.Enabled) return nullptr;
    const int port = Config.Port;
    auto& hostStr = Config.Host;
    auto& userStr = Config.Username;
    auto& passStr = Config.Password;
    const auto& rtspPath = Config.RtspPath;
    std::shared_ptr<ReolinkClient> client;

		// Fall back to RTSP URL credentials if PTZ-specific ones are empty
		// Note: we only take host/user/pass from RTSP URL, NOT port (RTSP uses 554, API uses 443/80)
		if (!rtspPath.empty() && (userStr.empty() || passStr.empty() || hostStr.empty()))
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
                std::unique_lock<std::shared_mutex> Lock(ctx.Mutex);
                auto [Existing, Inserted] = ctx.PtzClients.try_emplace(cameraId, client);
                if (!Inserted) client = Existing->second;
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
	const int linkedId = ReadPtzLinkedCameraId(ctx.Database, cameraId);

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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
	// Do not run HTTP discovery before a healthy Baichuan command (especially Stop).
	if (!PtzControlAllowed(m_GlobalContext->Database, cameraId, command == "stop"))
	{
		RespondNoPtz(res);
		return;
	}
	std::shared_ptr<ReolinkClient> client;
	const int deviceId = PtzLaneId(*m_GlobalContext, cameraId);

	// Try Baichuan client: direct camera first, then linked camera
	auto bcClient = m_GlobalContext->GetBaichuanClient(cameraId);
	// Safety-lane identity is separate from the existing linked transport fallback.
	const int linkedId = ReadPtzLinkedCameraId(m_GlobalContext->Database, cameraId);
	if (!bcClient && linkedId > 0) bcClient = m_GlobalContext->GetBaichuanClient(linkedId);

	// Parse speed from body if present
	int speed = 32;
	if (body && body.has("speed"))
	{
		if (body["speed"].t() != crow::json::type::Number ||
			body["speed"].d() < 1 || body["speed"].d() > 64 ||
			body["speed"].d() != static_cast<double>(static_cast<int>(body["speed"].d())))
		{
			res.code = 400; res.write("{\"error\":\"speed must be an integer from 1 to 64\"}"); res.end(); return;
		}
		speed = static_cast<int>(body["speed"].d());
	}

	// Handle preset command specially
	if (command == "preset")
	{
		if (!body || !body.has("id") || body["id"].t() != crow::json::type::Number ||
			body["id"].d() < 0 || body["id"].d() > 65535 ||
			body["id"].d() != static_cast<double>(static_cast<int>(body["id"].d())))
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
		bool ok = lease->Position(deviceId, [&] {
			bool sent = TryBaichuanPreset(bcClient, presetId);
			if (!sent) client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
			return sent || (client && client->PtzGoToPreset(presetId));
		});

		crow::json::wvalue result;
		result["success"] = ok;
		if (!ok) result["error"] = client ? client->GetLastError() : "No PTZ connection available";
		res.code = ok ? 200 : 500;
		res.write(result.dump());
		res.end();
		return;
	}

	PtzOp op = ParsePtzCommand(command);
	if (op == PtzOp::Stop && command != "stop")
	{
		res.code = 400; res.write("{\"error\":\"Unknown PTZ command\"}"); res.end(); return;
	}
	if (op == PtzOp::Stop)
	{
		// The lease owns the transport that may have moved the motor. Do not
		// delay its Stop by rediscovering a replacement or linked HTTP client.
		const auto stopped = lease->StopExisting(deviceId);
		if (stopped.has_value())
		{
			crow::json::wvalue result;
			result["success"] = *stopped;
			if (!*stopped) result["error"] = "Could not stop the active PTZ movement; retrying";
			res.code = *stopped ? 200 : 500;
			res.write(result.dump());
			res.end();
			return;
		}
	}
	if ((!bcClient || !bcClient->IsConnected()) && op != PtzOp::Stop)
	{
		client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
		if (!client) { RespondNoPtz(res); return; }
	}

	// Try Baichuan first (more reliable for Reolink cameras)
    struct Transport {
        std::shared_ptr<Witness::Camera::ReolinkBaichuanClient> Baichuan;
        std::shared_ptr<ReolinkClient> Http;
        bool BaichuanAttempted = false;
    };
    auto transport = std::make_shared<Transport>();
    transport->Baichuan = bcClient;
    transport->Http = client;
    auto send = [&] {
        transport->BaichuanAttempted = bcClient && bcClient->IsConnected();
        if (TryBaichuanPtz(bcClient, op, speed)) return true;
        if (!client) client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
        transport->Http = client;
        return client && client->PtzControl(op, speed);
    };
    // Retain clients, NOT context/database references, for later expiry/shutdown.
    auto stop = [transport] {
        bool attempted = false, ok = true;
        if (transport->BaichuanAttempted) {
            attempted = true;
            ok = TryBaichuanPtz(transport->Baichuan, PtzOp::Stop, 32) && ok;
        }
        if (transport->Http) { attempted = true; ok = transport->Http->PtzStop() && ok; }
        return attempted && ok;
    };
    bool ok;
    if (op == PtzOp::Stop) {
        // No lease may exist yet (e.g. emergency Stop following a server restart).
        // Resolve once here; any retry retains only transport clients.
        transport->BaichuanAttempted = bcClient && bcClient->IsConnected();
        if (!transport->BaichuanAttempted) transport->Http = GetPtzClientForCamera(*m_GlobalContext, cameraId);
        ok = lease->Stop(deviceId, stop);
    } else ok = lease->Move(deviceId, send, stop);

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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
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

	if (!body || !body.has("zoom") || body["zoom"].t() != crow::json::type::Number ||
		body["zoom"].d() < 0 || body["zoom"].d() > 65535 ||
		body["zoom"].d() != static_cast<double>(static_cast<int>(body["zoom"].d())))
	{
		res.code = 400;
		crow::json::wvalue err;
		err["error"] = "zoom must be an integer from 0 to 65535";
		res.write(err.dump());
		res.end();
		return;
	}

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
	if (!PtzControlAllowed(m_GlobalContext->Database, cameraId, false))
	{
		RespondNoPtz(res);
		return;
	}
	auto client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
	if (!client)
	{
		RespondNoPtz(res);
		return;
	}

	int zoomPos = (int)body["zoom"].i();
	bool ok = lease->Position(PtzLaneId(*m_GlobalContext, cameraId), [client, zoomPos] { return client->SetZoomPos(zoomPos); });

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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
	if (!PtzControlAllowed(m_GlobalContext->Database, cameraId, false))
	{
		RespondNoPtz(res);
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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
	if (!PtzControlAllowed(m_GlobalContext->Database, cameraId, false))
	{
		RespondNoPtz(res);
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
