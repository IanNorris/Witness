#pragma once

#include <string>
#include <cstdlib>

// Decode percent-encoded characters in a URL component (e.g., %40 -> @)
inline std::string UrlDecode(const std::string& input)
{
	std::string result;
	result.reserve(input.size());
	for (size_t i = 0; i < input.size(); ++i)
	{
		if (input[i] == '%' && i + 2 < input.size())
		{
			char hi = input[i + 1];
			char lo = input[i + 2];
			auto hexVal = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			int h = hexVal(hi), l = hexVal(lo);
			if (h >= 0 && l >= 0)
			{
				result += static_cast<char>((h << 4) | l);
				i += 2;
				continue;
			}
		}
		result += input[i];
	}
	return result;
}

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

	outUser = UrlDecode(creds.substr(0, colonPos));
	outPass = UrlDecode(creds.substr(colonPos + 1));

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
