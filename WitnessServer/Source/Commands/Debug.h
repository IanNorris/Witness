#pragma once

#include "../ListenerCommand.h"
#include "DebugBind.h"

class Command_Debug : public IListenerCommand
{
public:

	Command_Debug( DebugConsole* DebugConsoleInstance );

	void OnMessage( GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost ) override;

	void OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnSetMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnResetMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );

private:

	DebugConsole* DebugConsoleInstance;
};
