#pragma once

#include <memory>
#include <vector>
#include <string>

#include "cpprest/http_listener.h"
#include "cpprest/uri.h"

#include "Common.h"

#include "GlobalContext.h"

using namespace http;

class IListenerCommand
{
public:
	virtual void OnMessage( GlobalContext& Context, http_request& Message, const StringT& CurrentCommand, std::vector<StringT>& ChildPath, bool IsPost ) = 0;
};