#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "ReolinkClient.h"
#include "ReolinkBaichuanClient.h"
#include "SQLite.h"
#include "UrlHelpers.h"
#include "PtzMovementLease.h"
#include "PtzConfiguration.h"
#include "OnvifPtzClient.h"
#include <Log.h>
#include <cmath>
#ifndef WITNESS_CROW_ASYNC_RESPONSE_LIFETIME_PATCH
#error PTZ deferred responses require the reviewed Crow response lifetime patch
#endif

struct QueuedPtzConfiguration
{
    int CameraId = 0, LinkedId = 0;
    PtzConfiguration Camera, Linked;
};
static thread_local const QueuedPtzConfiguration* s_QueuedPtzConfiguration = nullptr;
static bool ValidateQueuedPtzConfiguration(GlobalContext& context, crow::response& response)
{
    const auto* expected = s_QueuedPtzConfiguration;
    if (!expected) return true; // Stop retains its original movement transport.
    if (ReadPtzConfiguration(context.Database, expected->CameraId) == expected->Camera &&
        ReadPtzLinkedCameraId(context.Database, expected->CameraId) == expected->LinkedId &&
        (expected->LinkedId <= 0 || ReadPtzConfiguration(context.Database, expected->LinkedId) == expected->Linked)) return true;
    response.code = 409;
    response.end("{\"error\":\"PTZ configuration changed while queued; retry\"}");
    return false;
}

void CrowListener::DispatchPtz(const crow::request& req, crow::response& res, int cameraId,
    PtzRequestExecutor::Kind kind,
    std::function<void(const crow::request&, crow::response&)> handler)
{
    // Authorize before admission, especially before Stop can cancel queued
    // gestures. Recheck in the handler when the queued work actually runs.
    const auto body = crow::json::load(req.body);
    const auto action = kind == PtzRequestExecutor::Kind::Read ? CrowAuth::Action::Read : CrowAuth::Action::ReadWrite;
    const bool admin = cameraId == 0 || req.url == "/ptz/" + std::to_string(cameraId) + "/preset/set" ||
        req.url == "/ptz/" + std::to_string(cameraId) + "/preset/delete";
    const auto privilege = admin ? CrowAuth::Privilege::Administrator : CrowAuth::Privilege::Normal;
    const int user = cameraId == 0 ? CrowAuth::IsAuthenticated(*m_GlobalContext, req, body ? &body : nullptr, action, privilege) :
        CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, body ? &body : nullptr, action, privilege, cameraId);
    if (user <= 0) { res.code = 403; res.end(); return; }
    std::shared_ptr<QueuedPtzConfiguration> expected;
    if (cameraId > 0 && kind != PtzRequestExecutor::Kind::Stop) {
        const auto config = ReadPtzConfiguration(m_GlobalContext->Database, cameraId);
        if (!config.Enabled) { res.code = 400; res.end("{\"error\":\"Camera does not have PTZ enabled\"}"); return; }
        const int linkedId = ReadPtzLinkedCameraId(m_GlobalContext->Database, cameraId);
        expected = std::make_shared<QueuedPtzConfiguration>();
        expected->CameraId = cameraId; expected->Camera = config; expected->LinkedId = linkedId;
        if (linkedId > 0) expected->Linked = ReadPtzConfiguration(m_GlobalContext->Database, linkedId);
        const bool linkedSupported = linkedId > 0 && (m_GlobalContext->GetPtzClient(linkedId) ||
            m_GlobalContext->GetBaichuanClient(linkedId) ||
            PtzDiscoverySupported(ReadPtzConfiguration(m_GlobalContext->Database, linkedId)));
        if (!PtzKnownUnsupported(config) && (!PtzDiscoverySupported(config) && !linkedSupported &&
            !m_GlobalContext->GetPtzClient(cameraId) && !m_GlobalContext->GetBaichuanClient(cameraId))) {
            res.code = 400; res.end("{\"error\":\"PTZ provider is not supported; only Reolink is currently implemented\"}"); return;
        }
    }
    if (!req.io_context) { res.code = 503; res.end(); return; }
    const int lane = cameraId == 0 ? 0 : ReadPtzLaneId(m_GlobalContext->Database, cameraId);
    // Own request strings/query parameters. Never mutate a live response from
    // an I/O worker: end() runs middleware and synchronous Crow socket writes.
    auto request = std::make_shared<crow::request>(req);
    request->middleware_context = nullptr;
    request->middleware_container = nullptr;
    auto sent = std::make_shared<std::atomic<bool>>(false);
    auto finish = [io = req.io_context, response = &res, sent, completions = m_PtzCompletions](int code, std::string payload) {
        if (sent->exchange(true)) return;
        completions->Add();
        try {
            asio::post(*io, [response, code, payload = std::move(payload), completions] {
                try {
                    response->code = code;
                    response->set_header("Content-Type", "application/json");
                    response->end(payload);
                } catch (...) { LOG_ERROR("PTZ response completion failed"); }
                completions->Done();
            });
        } catch (...) { completions->Done(); throw; }
    };
    auto cancel = [finish] { finish(409, "{\"error\":\"PTZ request cancelled or expired; retry if still needed\"}"); };
    auto run = [this, request, cameraId, lane, expected, handler = std::move(handler), finish] {
        struct Scope {
            const QueuedPtzConfiguration* Previous = s_QueuedPtzConfiguration;
            ~Scope() { s_QueuedPtzConfiguration = Previous; }
        } scope;
        s_QueuedPtzConfiguration = expected.get();
        try {
            if (cameraId > 0 && ReadPtzLaneId(m_GlobalContext->Database, cameraId) != lane) {
                finish(409, "{\"error\":\"PTZ camera links changed; retry\"}"); return;
            }
            crow::response result;
            handler(*request, result);
            finish(result.code, std::move(result.body));
        } catch (const std::exception& error) {
            LOG_ERROR("PTZ request failed for camera %d: %s", cameraId, error.what());
            finish(500, "{\"error\":\"PTZ request failed\"}");
        } catch (...) { finish(500, "{\"error\":\"PTZ request failed\"}"); }
    };
    if (!m_PtzRequests.Submit({lane, kind, std::move(run), std::move(cancel)})) {
        res.code = 503; res.set_header("Retry-After", "1");
        res.end("{\"error\":\"PTZ request queue is unavailable or full\"}");
    }
}

static std::shared_ptr<OnvifPtzClient> GetOnvifPtz(GlobalContext& ctx, int cameraId)
{
    auto config = ReadPtzConfiguration(ctx.Database, cameraId);
    if (!PtzKnownUnsupported(config)) return nullptr;
    std::string user, pass, host; int rtspPort = 0;
    ParseRtspUrl(config.RtspPath, user, pass, host, rtspPort);
    if (!config.Host.empty()) host = config.Host;
    if (!config.Username.empty()) user = config.Username;
    if (!config.Password.empty()) pass = config.Password;
    // Existing Reolink defaults are not ONVIF service ports.
    const int port = config.Port > 0 && config.Port != 443 && config.Port != 80 ? config.Port : 2020;
    if (host.empty() || user.empty() || pass.empty()) return nullptr;
    return OnvifPtzClient::Get(host, port, user, pass);
}

static int PtzLaneId(GlobalContext& ctx, int cameraId)
{
    return ReadPtzLaneId(ctx.Database, cameraId);
}

// Try to lazily initialize a PTZ client if it wasn't created at startup
static std::shared_ptr<ReolinkClient> TryInitPtzClient(GlobalContext& ctx, int cameraId)
{
    auto Config = ReadPtzConfiguration(ctx.Database, cameraId);
    if (!Config.Enabled || !PtzDiscoverySupported(Config)) return nullptr;
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
    if (PtzKnownUnsupported(ReadPtzConfiguration(ctx.Database, cameraId))) return nullptr;
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
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
	// Do not run HTTP discovery before a healthy Baichuan command (especially Stop).
	if (!PtzControlAllowed(m_GlobalContext->Database, cameraId, command == "stop"))
	{
		RespondNoPtz(res);
		return;
	}
	std::shared_ptr<ReolinkClient> client;
	const int deviceId = PtzLaneId(*m_GlobalContext, cameraId);

    if (PtzKnownUnsupported(ReadPtzConfiguration(m_GlobalContext->Database, cameraId))) {
        // A Stop always uses the retained transport first, including after a
        // configuration edit/disable. Failed acknowledgements remain retryable.
        auto retained = command == "stop" ? lease->StopExisting(deviceId) : std::optional<bool>{};
        auto onvif = retained.has_value() ? nullptr : GetOnvifPtz(*m_GlobalContext, cameraId);
        bool ok = false;
        std::string operationError;
        if (retained.has_value()) ok = *retained;
        else if (onvif) {
            int speed = 32;
            if (body && body.has("speed")) {
                if (body["speed"].t() != crow::json::type::Number || body["speed"].d()<1 || body["speed"].d()>64 || std::floor(body["speed"].d())!=body["speed"].d()) {
                    res.code=400; res.end("{\"error\":\"speed must be an integer from 1 to 64\"}"); return;
                }
                speed=static_cast<int>(body["speed"].d());
            }
            if (command == "stop") {
                // Discovery failure cannot have moved a motor: do not create a
                // perpetual safety-Stop retry for an unsupported/offline device.
                if(onvif->SupportsPanTilt()) ok=lease->Stop(deviceId,[onvif]{return onvif->Stop();});
            }
            else if (command == "preset") {
                if (!body || !body.has("id") || body["id"].t()!=crow::json::type::Number || body["id"].d()<1 || body["id"].d()>65535 || std::floor(body["id"].d())!=body["id"].d()) {
                    res.code=400;res.end("{\"error\":\"Invalid preset id\"}");return;
                }
                const int id=static_cast<int>(body["id"].d());
                ok=lease->Position(deviceId,[onvif,id]{return onvif->GoToPreset(id,[]{return !PtzRequestExecutor::MovementCancelled();});});
            } else {
                const auto op=ParsePtzCommand(command);
                if (op==PtzOp::Stop || op==PtzOp::ZoomInc || op==PtzOp::ZoomDec || op==PtzOp::FocusInc || op==PtzOp::FocusDec) {
                    res.code=400;res.end("{\"error\":\"This ONVIF transport supports pan/tilt only\"}");return;
                }
                if(onvif->SupportsPanTilt()) ok=lease->Move(deviceId,[onvif,op,speed,&operationError]{
                    const bool moved=onvif->Move(op,speed,[]{return !PtzRequestExecutor::MovementCancelled();});
                    // Capture before the lease's safety Stop can acknowledge
                    // successfully and clear the shared client's last error.
                    if(!moved) operationError=onvif->Error();
                    return moved;
                },[onvif]{return onvif->Stop();});
            }
        }
        crow::json::wvalue result; result["success"]=ok;
        if(!ok) {
            if(operationError.empty()) operationError=onvif?onvif->Error():"ONVIF PTZ unavailable";
            result["error"]=operationError.empty()?"ONVIF PTZ command failed":operationError;
        }
        res.code=PtzRequestExecutor::MovementCancelled()?409:ok?200:502;
        res.end(result.dump());return;
    }

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
			if (PtzRequestExecutor::MovementCancelled()) return false;
			bool sent = TryBaichuanPreset(bcClient, presetId);
			if (!sent) client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
			return sent || (!PtzRequestExecutor::MovementCancelled() && client &&
				client->PtzGoToPreset(presetId, [] { return !PtzRequestExecutor::MovementCancelled(); }));
		});

		crow::json::wvalue result;
		result["success"] = ok;
		if (!ok) result["error"] = client ? client->GetLastError() : "No PTZ connection available";
		res.code = PtzRequestExecutor::MovementCancelled() ? 409 : ok ? 200 : 500;
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
        if (PtzRequestExecutor::MovementCancelled()) return false;
        transport->BaichuanAttempted = bcClient && bcClient->IsConnected();
        if (TryBaichuanPtz(bcClient, op, speed)) return true;
        if (!client) client = GetPtzClientForCamera(*m_GlobalContext, cameraId);
        transport->Http = client;
        return !PtzRequestExecutor::MovementCancelled() && client &&
            client->PtzControl(op, speed, [] { return !PtzRequestExecutor::MovementCancelled(); });
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
        if (!transport->BaichuanAttempted && !transport->Http) { RespondNoPtz(res); return; }
        ok = lease->Stop(deviceId, stop);
    } else ok = lease->Move(deviceId, send, stop);

	crow::json::wvalue result;
	result["success"] = ok;
	if (!ok) result["error"] = client ? client->GetLastError() : "No PTZ connection available";
	res.code = PtzRequestExecutor::MovementCancelled() ? 409 : ok ? 200 : 500;
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
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
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
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
    if (PtzKnownUnsupported(ReadPtzConfiguration(m_GlobalContext->Database, cameraId))) {
        auto onvif=GetOnvifPtz(*m_GlobalContext,cameraId);
        const bool supported=onvif && onvif->SupportsPanTilt();
        crow::json::wvalue result; result["valid"]=false;result["provider"]="onvif";
        result["panTiltSupported"]=supported;result["zoomSupported"]=false;
        if(!supported) result["error"]=onvif?onvif->Error():"Missing ONVIF camera credentials";
        res.code=200;res.end(result.dump());return;
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
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
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
	bool ok = lease->Position(PtzLaneId(*m_GlobalContext, cameraId), [client, zoomPos] {
		return !PtzRequestExecutor::MovementCancelled() &&
			client->SetZoomPos(zoomPos, [] { return !PtzRequestExecutor::MovementCancelled(); });
	});

	crow::json::wvalue result;
	result["success"] = ok;
	if (!ok) result["error"] = client->GetLastError();
	res.code = PtzRequestExecutor::MovementCancelled() ? 409 : ok ? 200 : 500;
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
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
    if (PtzKnownUnsupported(ReadPtzConfiguration(m_GlobalContext->Database, cameraId))) {
        auto onvif=GetOnvifPtz(*m_GlobalContext,cameraId);
        std::vector<crow::json::wvalue> values;
        if(onvif) for(const auto& p:onvif->Presets()) {
            crow::json::wvalue value;value["id"]=p.Id;value["name"]=p.Name;value["enabled"]=p.Enabled;values.push_back(std::move(value));
        }
        crow::json::wvalue result;result["presets"]=std::move(values);
        if(onvif && !onvif->Error().empty()) result["error"]=onvif->Error();
        res.code=onvif && onvif->Error().empty()?200:502;res.end(result.dump());return;
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

	const auto lease = m_GlobalContext->PtzMovement;
	if (!lease) { res.code = 503; res.end(); return; }
	auto Control = lease->AcquireControl();
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
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
	if (!ValidateQueuedPtzConfiguration(*m_GlobalContext, res)) return;
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

    if (port == 2020) {
        auto onvif=OnvifPtzClient::Get(host,port,username,password);
        const bool ok=onvif && onvif->SupportsPanTilt();
        crow::json::wvalue result;result["success"]=ok;result["positionValid"]=false;
        result["message"]=ok?"ONVIF pan/tilt connected":onvif?onvif->Error():"ONVIF client capacity reached";
        res.code=200;res.end(result.dump());return;
    }

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
