// Keep these first: this target has no server PCH or Crow/SQLite dependency.
#include "Messages.h"
#include "TimerWorker.h"

#include <type_traits>

static_assert(std::is_base_of_v<Message, CameraSnapshotMessage>);
static_assert(std::is_base_of_v<Message, CameraBeginMotionMessage>);
static_assert(std::is_base_of_v<Message, CameraUpdateMotionMessage>);
static_assert(std::is_base_of_v<Message, CameraEndMotionMessage>);
static_assert(std::is_base_of_v<Message, CameraStartRecordMessage>);
static_assert(std::is_base_of_v<Message, CameraStopRecordMessage>);
static_assert(std::is_base_of_v<Message, CameraClipFinishedMessage>);
static_assert(std::is_base_of_v<Message, CameraStartupMessage>);
static_assert(std::is_base_of_v<Message, CameraReconnectMessage>);
static_assert(std::is_base_of_v<Message, CameraConnectedMessage>);
static_assert(std::is_base_of_v<Message, CameraWriteThumbnailMessage>);
static_assert(std::is_base_of_v<Message, CameraPreviewRequestMessage>);
static_assert(std::is_base_of_v<Message, CameraAddedMessage>);
static_assert(std::is_base_of_v<Message, CameraRemovedMessage>);
static_assert(std::is_base_of_v<WorkerBase, TimerWorker>);

int main()
{
    ClipStatistics stats;
    CameraBeginMotionMessage motion(8);
    CameraStartRecordMessage recording(8, 123, "private-test-only");
    CameraReconnectMessage reconnect(8, "test reconnect");
    return stats.TimestampClipStarted == INT64_MAX && stats.TimestampMotionEnded == 0
        && motion.Camera == 8 && motion.Result.ClassificationSuperset == 0
        && recording.Timestamp == 123 && recording.Path == "private-test-only"
        && reconnect.Error == "test reconnect" ? 0 : 1;
}
