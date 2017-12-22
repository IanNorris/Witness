#pragma once

#include "../ListenerCommand.h"

class Command_Static : public IListenerCommand
{
public:

	Command_Static( json::object& Config );

	void OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

private:

	unordered_map<string_t,string_t> m_staticDataPaths;
	string_t m_root;
};