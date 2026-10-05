// Include this first: the server's Crow PCH must not supply missing declarations.
#include "GlobalContext.h"
#include <type_traits>

template<class T> concept CompleteType = requires { sizeof(T); };
static_assert(!CompleteType<EventBroadcaster>);
static_assert(!CompleteType<StreamBroadcaster>);
static_assert(std::is_same_v<decltype(GlobalContext::Events), std::shared_ptr<EventBroadcaster>>);
static_assert(std::is_same_v<decltype(GlobalContext::Streams), std::shared_ptr<StreamBroadcaster>>);

int main()
{
    GlobalContext Context;
    // Construction remains eager and owns the same services; no server, socket,
    // broadcaster thread, database or camera is started by this test.
    return Context.LongPoll && Context.Events && Context.Streams ? 0 : 1;
}
