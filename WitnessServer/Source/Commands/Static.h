#pragma once

#include "../ListenerCommand.h"

class Command_Static : public IListenerCommand
{
public:

	Command_Static( const std::unordered_map< StringT, StringT >& Settings );

	void OnMessage( GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost ) override;

private:

	std::unordered_map<StringT,StringT> m_staticDataPaths;
	StringT m_root;
};