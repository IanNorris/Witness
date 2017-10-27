#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"

#include <iostream>

using namespace web::json;
using namespace web::http::client;

void Command_Authenticate::OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	//Registration:
	//Usernames are generated in advance, user claims it with a one-time password
	//Client sends the server its public key, which it stores if user has not been seen before
	//One time password is erased

	//Change of password:
	//Client sends its new public key, and signs the request with its old one

	//Login:

	//Server stores Ed25519 public key for username
	//Client (JS) generates public key from username + password combination
	//Client sends a login packet and gets an PK encrypted packet with a nonce as a response. 
	//Client signs the packet and sends it back

	Message.reply( status_codes::OK );
}
