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

	static string_t GetSessionToken( const http_request& Message, uint16_t PortIn );

	static bool IsAuthenticated( const GlobalContext& Context, http_request& Message, const json::value& Packet, Action ActionType, Privilege RequiredPrivilege );

	void OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnLoginMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnLogoutMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnGetProfileMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnEnumUsersMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnNewUserMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnChangePasswordMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );

private:

	uint16_t Port;

};

string_t GetRandomToken();

void OfflineCreationForFirstUser( const GlobalContext& Context );
