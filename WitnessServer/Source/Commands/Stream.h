#pragma once

#include "../ListenerCommand.h"

class Command_Stream : public IListenerCommand
{
public:

	void OnMessage(GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost) override;

	void OnSegmentMessage( const GlobalContext& Context, http_request& Message, const StringT& TargetCamera, const StringT& TargetSegment, const StringT& TargetPart, const json::value& Packet );
	void OnPlaylistMessage( const GlobalContext& Context, http_request& Message, const StringT& TargetCamera, const json::value& Packet );
};

StringT GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video);
