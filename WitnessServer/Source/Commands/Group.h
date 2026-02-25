#pragma once

#include "../ListenerCommand.h"

class Command_Group : public IListenerCommand
{
public:

	void OnMessage( GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost ) override;

	void OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnCreateMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnUpdateMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnDeleteMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
};
