#include "GlobalContext.h"
#include "EventBroadcaster.h"
#include "StreamBroadcaster.h"
#include <ContinuousOutputStream.h>
#include <mutex>

GlobalContext::GlobalContext()
    : Mutex()
    , LongPoll(std::make_shared<LongPollDispatch>())
    , Events(std::make_shared<EventBroadcaster>())
    , Streams(std::make_shared<StreamBroadcaster>())
{}

void GlobalContext::RegisterContinuousStream(int cameraId, const std::shared_ptr<Witness::Camera::ContinuousOutputStream>& stream)
{
    std::unique_lock lock(ContinuousStreamMutex);
    ContinuousStreams[cameraId] = stream;
}

Witness::Camera::ContinuousSegmentSnapshot GlobalContext::GetActiveDvrSegment(int cameraId) const
{
    std::shared_ptr<Witness::Camera::ContinuousOutputStream> stream;
    {
        std::shared_lock lock(ContinuousStreamMutex);
        const auto found = ContinuousStreams.find(cameraId);
        if (found != ContinuousStreams.end()) stream = found->second.lock();
    }
    return stream ? stream->GetCommittedSegment() : Witness::Camera::ContinuousSegmentSnapshot{};
}
