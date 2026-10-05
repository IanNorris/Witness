#include "GlobalContext.h"
#include "EventBroadcaster.h"
#include "StreamBroadcaster.h"

GlobalContext::GlobalContext()
    : Mutex()
    , LongPoll(std::make_shared<LongPollDispatch>())
    , Events(std::make_shared<EventBroadcaster>())
    , Streams(std::make_shared<StreamBroadcaster>())
{}
