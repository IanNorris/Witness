#pragma once

#include "../ListenerCommand.h"

class Command_Stream : public IListenerCommand
{
public:

	void OnMessage(GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, std::vector<string_t>& ChildPath, bool IsPost) override;

	void OnSegmentMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const string_t& TargetSegment, const json::value& Packet );
	void OnPlaylistMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet );
};

string_t GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video);
