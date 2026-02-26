#pragma once

#include "../ListenerCommand.h"

class Command_Authenticate : public IListenerCommand
{
public:

	enum class Action
	{
		Read,
		ReadWrite
	};

	enum class Privilege
	{
		Normal,
		Administrator
	};

	Command_Authenticate( uint16_t Port ) : Port( Port ) {}

	static StringT GetSessionToken( const http_request& Message, uint16_t PortIn );

	static int IsAuthenticated( const GlobalContext& Context, http_request& Message, const json::value& Packet, Action ActionType, Privilege RequiredPrivilege );

	static int IsCameraAuthenticated( const GlobalContext& Context, http_request& Message, const json::value& Packet, Action ActionType, Privilege RequiredPrivilege, int CameraUID );

	void OnMessage( GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost ) override;

	void OnLoginMessage( const GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost );
	void OnLogoutMessage( const GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost );
	void OnGetProfileMessage( const GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost );
	void OnEnumUsersMessage( const GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost );
	void OnNewUserMessage( const GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost );
	void OnChangePasswordMessage( const GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost );
	void OnToggleEnabledMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnToggleAdminMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnSetDisplayNameMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );
	void OnSetUserGroupsMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet );

private:

	uint16_t Port;

};

StringT GetRandomToken();

StringT GetHashedPasswordKey_Algorithm0( const StringT& Username, const StringT Password );
bool CheckHashedPasswordKey_Algorithm0( const StringT& Key, const StringT& Username, const StringT Password );

void OfflineCreationForFirstUser( const GlobalContext& Context );
