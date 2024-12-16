#pragma once

#include "../ListenerCommand.h"

class Command_Static : public IListenerCommand
{
public:

	Command_Static( const std::unordered_map< string_t, string_t >& Settings );

	void OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, std::vector<string_t>& ChildPath, bool IsPost ) override;

private:

	std::unordered_map<string_t,string_t> m_staticDataPaths;
	string_t m_root;
};