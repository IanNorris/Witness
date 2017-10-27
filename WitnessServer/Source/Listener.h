#pragma once

#define WITNESS_LISTENER_VERSION "0.0.0.1"

#include <unordered_map>

#include "cpprest/json.h"
#include "cpprest/http_listener.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"

#include "ListenerCommand.h"

using namespace http::experimental::listener;

class WitnessListener
{
public:
	WitnessListener( utility::string_t Hostname, int Port );
	virtual ~WitnessListener();

	void Initialise( json::object& Config );

	void Start();

	void Stop();

private:

	void OnCommand( http_request Message, bool IsPost );

	unique_ptr<http_listener> m_Listener;
	unique_ptr<GlobalContext> m_GlobalContext;

	unordered_map<string_t, unique_ptr<IListenerCommand>> m_Commands;
};
