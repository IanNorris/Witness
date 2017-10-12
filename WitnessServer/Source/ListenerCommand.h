#pragma once

#include <memory>
#include <vector>
#include <string>

#include "cpprest/http_listener.h"
#include "cpprest/uri.h"

#include "GlobalContext.h"

using namespace std;
using namespace web;
using namespace http;
using namespace utility;

class IListenerCommand
{
public:
	virtual void OnMessage( const unique_ptr<GlobalContext>& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost ) = 0;
};