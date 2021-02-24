#pragma once

#include "../ListenerCommand.h"

class Command_Camera : public IListenerCommand
{
public:

	void OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnPreviewMessage( GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet, bool LargePreview );

	void OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet, bool AsAdmin, bool LongPoll );

	void OnRecordMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet );

	void OnCreateMessage(const GlobalContext& Context, http_request& Message, const json::value& Packet);

	void OnDeleteMessage(const GlobalContext& Context, http_request& Message, const json::value& Packet);

	void OnSetGroupsMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );

	void OnResetStatsMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
};
