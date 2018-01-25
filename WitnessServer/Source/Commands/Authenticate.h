#pragma once

#include "../ListenerCommand.h"

class Command_Authenticate : public IListenerCommand
{
public:

	static string_t GetSessionToken( const http_request& Message );

	static bool IsAuthenticated( const unique_ptr<GlobalContext>& Context, http_request& Message, const json::value& Packet );

	void OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;

	void OnLoginMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnLogoutMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
	void OnGetProfileMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost );
};

string_t GetRandomToken();

void OfflineCreationForFirstUser( const unique_ptr<GlobalContext>& Context );
