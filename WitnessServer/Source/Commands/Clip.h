#pragma once

#include "../ListenerCommand.h"

class Command_Clip : public IListenerCommand
{
public:

	void OnMessage( GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost ) override;

	void OnThumbnailMessage( const GlobalContext& Context, http_request& Message, bool Video, const StringT& TargetCamera, const StringT& TargetClip, const json::value& Packet );

	void OnEnumClipsMessage( const GlobalContext& Context, http_request& Message, const StringT& TargetCamera, const StringT& MaxCount, const StringT& StartDate, const StringT& RangePeriod, const StringT& PageOffset, const json::value& Packet );

	void OnToggleSaveMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnDeleteMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );

	static void DeleteOldClips( const GlobalContext& Context, int DaysToDelete );
};

StringT GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video);
