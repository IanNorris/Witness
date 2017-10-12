#include "Authenticate.h"

void Command_Authenticate::OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	Message.reply( status_codes::OK );
}