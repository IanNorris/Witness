#pragma once

#include "../ListenerCommand.h"

class Command_Authenticate : public IListenerCommand
{
public:

	void OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) override;
};

string_t GetRandomToken();

void OfflineCreationForFirstUser( const unique_ptr<GlobalContext>& Context );
