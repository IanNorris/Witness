#pragma once

struct SettingsMap
{
	StringT Name;
	std::unordered_map<StringT, StringT> Settings;
};
