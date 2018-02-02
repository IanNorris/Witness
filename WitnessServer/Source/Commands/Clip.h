#pragma once

#include "../ListenerCommand.h"

class Command_Clip : public IListenerCommand
{
public:

	void OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnThumbnailMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& TargetCamera, const string_t& TargetClip, const json::value& Packet );
};
