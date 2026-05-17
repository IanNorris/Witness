#pragma once

#include <string>
#include <cstdlib>

// Parse credentials and host from an RTSP URL: rtsp://user:pass@host:port/path
inline void ParseRtspUrl(const std::string& url, std::string& outUser, std::string& outPass, std::string& outHost, int& outPort)
{
	auto schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos) return;
	size_t authStart = schemeEnd + 3;

	auto atPos = url.find('@', authStart);
	if (atPos == std::string::npos) return;

	// Extract user:pass
	std::string creds = url.substr(authStart, atPos - authStart);
	auto colonPos = creds.find(':');
	if (colonPos == std::string::npos) return;

	outUser = creds.substr(0, colonPos);
	outPass = creds.substr(colonPos + 1);

	// Extract host:port
	size_t hostStart = atPos + 1;
	auto pathPos = url.find('/', hostStart);
	std::string hostPort = (pathPos != std::string::npos)
		? url.substr(hostStart, pathPos - hostStart)
		: url.substr(hostStart);

	auto hpColon = hostPort.find(':');
	if (hpColon != std::string::npos)
	{
		outHost = hostPort.substr(0, hpColon);
		outPort = std::atoi(hostPort.substr(hpColon + 1).c_str());
	}
	else
	{
		outHost = hostPort;
	}
}
