#pragma once

#include "cpprest/json.h"
#include "cpprest/http_listener.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"

using namespace std;
using namespace web;
using namespace http;
using namespace utility;
using namespace http::experimental::listener;

class WitnessListener
{
public:
	WitnessListener( string_t Address );
	virtual ~WitnessListener();

	void Start();

	void Stop();

private:

	void OnGET( http_request Message );
	void OnPOST( http_request Message );

	unique_ptr<http_listener> m_Listener;
};
