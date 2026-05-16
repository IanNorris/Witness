#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "ReolinkClient.h"
#include <Log.h>
#include <crow/json.h>
#include <sstream>

ReolinkClient::ReolinkClient(const std::string& host, int port, bool useTls, const std::string& username, const std::string& password)
	: m_Host(host)
	, m_Port(port)
	, m_Username(username)
	, m_Password(password)
	, m_TokenExpiry(std::chrono::steady_clock::now())
{
	std::string url = (useTls ? "https://" : "http://") + m_Host + ":" + std::to_string(m_Port);
	m_HttpClient = std::make_unique<httplib::Client>(url);
	m_HttpClient->set_connection_timeout(5);
	m_HttpClient->set_read_timeout(10);
	if (useTls)
	{
		m_HttpClient->enable_server_certificate_verification(false);
	}
}

ReolinkClient::~ReolinkClient() = default;

std::shared_ptr<ReolinkClient> ReolinkClient::AutoDetect(const std::string& host, int port, const std::string& username, const std::string& password)
{
	// Try HTTPS on the given port first
	auto client = std::make_shared<ReolinkClient>(host, port, true, username, password);
	if (client->Login())
	{
		LOG_INFO("ReolinkClient: Connected via HTTPS to %s:%d", host.c_str(), port);
		return client;
	}

	// Try HTTP on the given port
	client = std::make_unique<ReolinkClient>(host, port, false, username, password);
	if (client->Login())
	{
		LOG_INFO("ReolinkClient: Connected via HTTP to %s:%d", host.c_str(), port);
		return client;
	}

	// If port wasn't 443, try HTTPS on 443
	if (port != 443)
	{
		client = std::make_unique<ReolinkClient>(host, 443, true, username, password);
		if (client->Login())
		{
			LOG_INFO("ReolinkClient: Connected via HTTPS to %s:443", host.c_str());
			return client;
		}
	}

	// If port wasn't 80, try HTTP on 80
	if (port != 80)
	{
		client = std::make_unique<ReolinkClient>(host, 80, false, username, password);
		if (client->Login())
		{
			LOG_INFO("ReolinkClient: Connected via HTTP to %s:80", host.c_str());
			return client;
		}
	}

	LOG_ERROR("ReolinkClient: Could not connect to %s (tried HTTPS and HTTP)", host.c_str());
	return nullptr;
}

bool ReolinkClient::Login()
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	std::string body = R"([{"cmd":"Login","action":0,"param":{"User":{"userName":")" +
		m_Username + R"(","password":")" + m_Password + R"("}}}])";

	auto result = m_HttpClient->Post("/api.cgi?cmd=Login", body, "application/json");
	if (!result || result->status != 200)
	{
		m_LastError = "Login request failed: " + (result ? std::to_string(result->status) : "connection error");
		LOG_ERROR("ReolinkClient: %s (%s:%d)", m_LastError.c_str(), m_Host.c_str(), m_Port);
		return false;
	}

	auto json = crow::json::load(result->body);
	if (!json || json.size() == 0)
	{
		m_LastError = "Login: invalid JSON response";
		LOG_ERROR("ReolinkClient: %s", m_LastError.c_str());
		return false;
	}

	auto& resp = json[0];
	if (resp.has("value") && resp["value"].has("Token") && resp["value"]["Token"].has("name"))
	{
		m_Token = resp["value"]["Token"]["name"].s();
		// Token valid for ~1 hour, refresh at 50 minutes
		m_TokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(50);
		LOG_INFO("ReolinkClient: Logged in to %s:%d", m_Host.c_str(), m_Port);
		return true;
	}

	// Check for error code
	int code = 0;
	if (resp.has("code"))
		code = (int)resp["code"].i();

	m_LastError = "Login failed, code=" + std::to_string(code);
	LOG_ERROR("ReolinkClient: %s (%s:%d)", m_LastError.c_str(), m_Host.c_str(), m_Port);
	return false;
}

std::string ReolinkClient::SendCommand(const std::string& cmd, const std::string& body)
{
	std::lock_guard<std::mutex> lock(m_Mutex);

	if (m_Token.empty() || std::chrono::steady_clock::now() >= m_TokenExpiry)
	{
		LOG_INFO("ReolinkClient: Token expired/empty for %s, re-authenticating...", cmd.c_str());
		// Login inline (mutex already held)
		std::string loginBody = R"([{"cmd":"Login","action":0,"param":{"User":{"userName":")" +
			m_Username + R"(","password":")" + m_Password + R"("}}}])";

		auto loginResult = m_HttpClient->Post("/api.cgi?cmd=Login", loginBody, "application/json");
		if (!loginResult || loginResult->status != 200)
		{
			m_LastError = "Login failed in SendCommand: " + (loginResult ? ("HTTP " + std::to_string(loginResult->status)) : "connection error");
			LOG_ERROR("ReolinkClient: %s", m_LastError.c_str());
			return "";
		}

		auto json = crow::json::load(loginResult->body);
		if (!json || json.size() == 0)
		{
			m_LastError = "Login: invalid JSON in SendCommand";
			return "";
		}

		auto& resp = json[0];
		if (resp.has("value") && resp["value"].has("Token") && resp["value"]["Token"].has("name"))
		{
			m_Token = resp["value"]["Token"]["name"].s();
			m_TokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(50);
		}
		else
		{
			m_LastError = "Login failed: no token in response";
			return "";
		}
	}

	std::string path = "/api.cgi?cmd=" + cmd + "&token=" + m_Token;
	auto result = m_HttpClient->Post(path, body, "application/json");

	if (!result)
	{
		m_LastError = cmd + ": connection error";
		return "";
	}

	if (result->status == 401)
	{
		// Token expired, retry login once
		m_Token.clear();

		std::string loginBody = R"([{"cmd":"Login","action":0,"param":{"User":{"userName":")" +
			m_Username + R"(","password":")" + m_Password + R"("}}}])";

		auto loginResult = m_HttpClient->Post("/api.cgi?cmd=Login", loginBody, "application/json");
		if (!loginResult || loginResult->status != 200)
		{
			m_LastError = cmd + ": re-login failed";
			return "";
		}

		auto json = crow::json::load(loginResult->body);
		if (json && json.size() > 0)
		{
			auto& resp = json[0];
			if (resp.has("value") && resp["value"].has("Token") && resp["value"]["Token"].has("name"))
			{
				m_Token = resp["value"]["Token"]["name"].s();
				m_TokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(50);
			}
		}

		if (m_Token.empty())
		{
			m_LastError = cmd + ": re-login failed (no token)";
			return "";
		}

		path = "/api.cgi?cmd=" + cmd + "&token=" + m_Token;
		result = m_HttpClient->Post(path, body, "application/json");
		if (!result || result->status != 200)
		{
			m_LastError = cmd + ": retry failed";
			return "";
		}
	}

	if (result->status != 200)
	{
		m_LastError = cmd + ": HTTP " + std::to_string(result->status);
		return "";
	}

	return result->body;
}

const char* ReolinkClient::PtzOpToString(PtzOp op)
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
	case PtzOp::ToPos:     return "ToPos";
	default:               return "Stop";
	}
}

bool ReolinkClient::PtzControl(PtzOp op, int speed)
{
	if (speed < 1) speed = 1;
	if (speed > 64) speed = 64;

	std::string body = R"([{"cmd":"PtzCtrl","action":0,"param":{"channel":0,"op":")" +
		std::string(PtzOpToString(op)) +
		R"(","speed":)" + std::to_string(speed) + R"(}}])";

	std::string response = SendCommand("PtzCtrl", body);
	if (response.empty())
	{
		// For zoom ops, try alternative names (some firmware uses ZoomIn/ZoomOut)
		if (op == PtzOp::ZoomInc || op == PtzOp::ZoomDec)
		{
			const char* altOp = (op == PtzOp::ZoomInc) ? "ZoomIn" : "ZoomOut";
			body = R"([{"cmd":"PtzCtrl","action":0,"param":{"channel":0,"op":")" +
				std::string(altOp) + R"(","speed":)" + std::to_string(speed) + R"(}}])";
			response = SendCommand("PtzCtrl", body);
		}
		if (response.empty())
		{
			LOG_ERROR("ReolinkClient::PtzControl: empty response, lastError=%s", m_LastError.c_str());
			return false;
		}
	}

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
	{
		m_LastError = "PtzCtrl: invalid JSON: " + response.substr(0, 200);
		LOG_ERROR("ReolinkClient: %s", m_LastError.c_str());
		return false;
	}

	// Check for success (code 0)
	auto& resp = json[0];
	if (resp.has("code") && resp["code"].i() != 0)
	{
		int code = (int)resp["code"].i();

		// For zoom ops, try alternative names on failure
		if ((op == PtzOp::ZoomInc || op == PtzOp::ZoomDec) && code != 0)
		{
			const char* altOp = (op == PtzOp::ZoomInc) ? "ZoomIn" : "ZoomOut";
			body = R"([{"cmd":"PtzCtrl","action":0,"param":{"channel":0,"op":")" +
				std::string(altOp) + R"(","speed":)" + std::to_string(speed) + R"(}}])";
			response = SendCommand("PtzCtrl", body);
			if (!response.empty())
			{
				json = crow::json::load(response);
				if (json && json.size() > 0)
				{
					auto& retryResp = json[0];
					if (!retryResp.has("code") || retryResp["code"].i() == 0)
						return true;
				}
			}
		}

		m_LastError = "PtzCtrl failed, code=" + std::to_string(code);
		if (resp.has("error") && resp["error"].has("detail"))
			m_LastError += " detail=" + std::string(resp["error"]["detail"].s());
		LOG_ERROR("ReolinkClient: %s (response: %s)", m_LastError.c_str(), response.substr(0, 300).c_str());
		return false;
	}

	return true;
}

bool ReolinkClient::PtzStop()
{
	return PtzControl(PtzOp::Stop);
}

bool ReolinkClient::PtzGoToPreset(int presetId)
{
	std::string body = R"([{"cmd":"PtzCtrl","action":0,"param":{"channel":0,"op":"ToPos","id":)" +
		std::to_string(presetId) + R"(,"speed":32}}])";

	std::string response = SendCommand("PtzCtrl", body);
	if (response.empty())
		return false;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return false;

	auto& resp = json[0];
	return !resp.has("code") || resp["code"].i() == 0;
}

PtzPosition ReolinkClient::GetPosition()
{
	PtzPosition pos;

	std::string body = R"([{"cmd":"GetPtzCurPos","action":0,"param":{"channel":0}}])";
	std::string response = SendCommand("GetPtzCurPos", body);
	if (response.empty())
		return pos;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return pos;

	auto& resp = json[0];
	if (resp.has("value") && resp["value"].has("Ppos") && resp["value"].has("Tpos"))
	{
		pos.Pan = (int)resp["value"]["Ppos"].i();
		pos.Tilt = (int)resp["value"]["Tpos"].i();
		pos.Valid = true;
	}

	return pos;
}

ReolinkClient::ZoomFocusState ReolinkClient::GetZoomFocus()
{
	ZoomFocusState state;

	std::string body = R"([{"cmd":"GetZoomFocus","action":0,"param":{"channel":0}}])";
	std::string response = SendCommand("GetZoomFocus", body);
	if (response.empty())
		return state;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return state;

	auto& resp = json[0];
	if (resp.has("value") && resp["value"].has("ZoomFocus"))
	{
		auto& zf = resp["value"]["ZoomFocus"];
		if (zf.has("zoom") && zf["zoom"].has("pos"))
			state.ZoomPos = (int)zf["zoom"]["pos"].i();
		if (zf.has("focus") && zf["focus"].has("pos"))
			state.FocusPos = (int)zf["focus"]["pos"].i();
		state.Valid = true;
	}

	// Also try to get zoom range
	std::string abilityBody = R"([{"cmd":"GetAbility","action":0,"param":{"User":{"userName":")" +
		m_Username + R"("}}}])";
	std::string abilityResponse = SendCommand("GetAbility", abilityBody);
	if (!abilityResponse.empty())
	{
		auto abilityJson = crow::json::load(abilityResponse);
		if (abilityJson && abilityJson.size() > 0)
		{
			auto& aResp = abilityJson[0];
			if (aResp.has("value") && aResp["value"].has("Ability") &&
				aResp["value"]["Ability"].has("ptzCtrl") &&
				aResp["value"]["Ability"]["ptzCtrl"].has("zoomMax"))
			{
				state.ZoomMax = (int)aResp["value"]["Ability"]["ptzCtrl"]["zoomMax"].i();
			}
		}
	}

	// Fallback: if we didn't get zoomMax from ability, try common defaults
	if (state.ZoomMax == 0)
		state.ZoomMax = 3200;  // Common Reolink default (32x = 3200 at 100 per 1x)

	return state;
}

bool ReolinkClient::SetZoomPos(int zoomPos)
{
	std::string body = R"([{"cmd":"StartZoomFocus","action":0,"param":{"channel":0,"op":"ZoomPos","pos":{"zoom":{"pos":)" +
		std::to_string(zoomPos) + R"(}}}}])";

	std::string response = SendCommand("StartZoomFocus", body);
	if (response.empty())
	{
		LOG_ERROR("ReolinkClient::SetZoomPos: empty response, lastError=%s", m_LastError.c_str());
		return false;
	}

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return false;

	auto& resp = json[0];
	if (resp.has("code") && resp["code"].i() != 0)
	{
		m_LastError = "SetZoomPos failed, code=" + std::to_string((int)resp["code"].i());
		LOG_ERROR("ReolinkClient: %s (response: %s)", m_LastError.c_str(), response.substr(0, 300).c_str());
		return false;
	}
	return true;
}

bool ReolinkClient::SetFocusPos(int focusPos)
{
	std::string body = R"([{"cmd":"StartZoomFocus","action":0,"param":{"channel":0,"op":"FocusPos","pos":{"focus":{"pos":)" +
		std::to_string(focusPos) + R"(}}}}])";

	std::string response = SendCommand("StartZoomFocus", body);
	if (response.empty())
		return false;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return false;

	auto& resp = json[0];
	if (resp.has("code") && resp["code"].i() != 0)
	{
		m_LastError = "SetFocusPos failed, code=" + std::to_string((int)resp["code"].i());
		return false;
	}
	return true;
}

std::vector<PtzPreset> ReolinkClient::GetPresets()
{
	std::vector<PtzPreset> presets;

	std::string body = R"([{"cmd":"GetPtzPreset","action":0,"param":{"channel":0}}])";
	std::string response = SendCommand("GetPtzPreset", body);
	if (response.empty())
		return presets;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return presets;

	auto& resp = json[0];
	if (resp.has("value") && resp["value"].has("PtzPreset"))
	{
		auto& presetArray = resp["value"]["PtzPreset"];
		for (size_t i = 0; i < presetArray.size(); i++)
		{
			auto& p = presetArray[i];
			PtzPreset preset;
			preset.Id = (int)p["id"].i();
			if (p.has("name"))
				preset.Name = p["name"].s();
			preset.Enabled = p.has("enable") && p["enable"].i() != 0;
			if (preset.Enabled)
				presets.push_back(preset);
		}
	}

	return presets;
}

bool ReolinkClient::SetPreset(int id, const std::string& name)
{
	std::string body = R"([{"cmd":"SetPtzPreset","action":0,"param":{"channel":0,"enable":1,"id":)" +
		std::to_string(id) + R"(,"name":")" + name + R"("}}])";

	std::string response = SendCommand("SetPtzPreset", body);
	if (response.empty())
		return false;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return false;

	auto& resp = json[0];
	return !resp.has("code") || resp["code"].i() == 0;
}

bool ReolinkClient::DeletePreset(int id)
{
	std::string body = R"([{"cmd":"SetPtzPreset","action":0,"param":{"channel":0,"enable":0,"id":)" +
		std::to_string(id) + R"(}}])";

	std::string response = SendCommand("SetPtzPreset", body);
	return !response.empty();
}

bool ReolinkClient::GetMotionState(int channel)
{
	std::string body = R"([{"cmd":"GetMdState","action":0,"param":{"channel":)" +
		std::to_string(channel) + R"(}}])";

	std::string response = SendCommand("GetMdState", body);
	if (response.empty())
		return false;

	auto json = crow::json::load(response);
	if (!json || json.size() == 0)
		return false;

	auto& resp = json[0];
	if (resp.has("value") && resp["value"].has("state"))
		return resp["value"]["state"].i() != 0;

	return false;
}

bool ReolinkClient::IsReachable()
{
	auto result = m_HttpClient->Get("/");
	return result && result->status != 0;
}
