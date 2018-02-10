#pragma once

#include "../ListenerCommand.h"

class Command_Camera : public IListenerCommand
{
public:

	void OnMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnPreviewMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet );

	void OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );

	void OnRecordMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet );
};
