#pragma once

#include "../ListenerCommand.h"

class Command_Authenticate : public IListenerCommand
{
public:

	static string_t GetSessionToken( const http_request& Message );

	static bool IsAuthenticated( const GlobalContext& Context, http_request& Message, const json::value& Packet, bool RequireCSRF );

	void OnMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnLoginMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnLogoutMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnGetProfileMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
};

string_t GetRandomToken();

void OfflineCreationForFirstUser( const GlobalContext& Context );
