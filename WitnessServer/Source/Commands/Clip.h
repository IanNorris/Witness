#pragma once

#include "../ListenerCommand.h"

class Command_Clip : public IListenerCommand
{
public:

	void OnMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnThumbnailMessage( const GlobalContext& Context, http_request& Message, bool Video, const string_t& TargetCamera, const string_t& TargetClip, const json::value& Packet );

	void OnEnumClipsMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const string_t& MaxCount, const string_t& StartDate, const string_t& RangePeriod, const string_t& Page, const json::value& Packet );
};

string_t GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video);
