find_package(Crow CONFIG REQUIRED)
include("${CMAKE_SOURCE_DIR}/Tools/CrowAsyncResponsePatch.cmake")
find_package(FFMPEG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
include_directories("${CMAKE_SOURCE_DIR}/ThirdParty/tinyxml2")
add_executable(OnvifPtzProbe EXCLUDE_FROM_ALL
    ../Tools/Tests/OnvifPtzProbe.cpp Source/OnvifPtzClient.cpp ../ThirdParty/tinyxml2/tinyxml2.cpp)
target_include_directories(OnvifPtzProbe PRIVATE Source ${CMAKE_SOURCE_DIR}/ThirdParty/httplib)
target_link_libraries(OnvifPtzProbe PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(MSVC)
    target_compile_options(OnvifPtzProbe PRIVATE /utf-8 /W3)
endif()

set(SERVER_SOURCES
    Source/BuildIdentity.cpp
    Source/AudioIntelligenceWorker.cpp
    Source/AsyncWorker.cpp
    Source/AuthHelpers.cpp
    Source/ClipHelpers.cpp
    Source/ConfigPaths.cpp
    Source/ClientHealthHistory.cpp
    Source/CpuUsage.cpp
    Source/DetectionAssetFiles.cpp
    Source/DetectionCleanup.cpp
    Source/ClipReprocessWorker.cpp
    Source/CrowAuth.cpp
    Source/CrowListener.cpp
    Source/CrowRoutes_Auth.cpp
    Source/CrowRoutes_Api.cpp
    Source/CrowRoutes_Camera.cpp
    Source/CrowRoutes_Clip.cpp
    Source/CrowRoutes_Debug.cpp
    Source/CrowRoutes_Face.cpp
    Source/CrowRoutes_FaceRecognition.cpp
    Source/CrowRoutes_DVR.cpp
    Source/CrowRoutes_Group.cpp
    Source/CrowRoutes_Ptz.cpp
    Source/PtzRequestExecutor.cpp
    Source/RecentCaptureJobs.cpp
    Source/CrowRoutes_RecentCapture.cpp
    Source/CrowRoutes_Action.cpp
    Source/CrowRoutes_Setup.cpp
    Source/CrowRoutes_Stream.cpp
    Source/CrowRoutes_Tag.cpp
    Source/CameraWorker.cpp
    Source/Common.cpp
    Source/Database.cpp
    Source/FaceRecognitionCache.cpp
    Source/GlobalContext.cpp
    Source/ImageProcessorWorker.cpp
    Source/HttpServerDiagnostics.cpp
    Source/Init.cpp
    Source/JsonHelpers.cpp
    Source/Main.cpp
    Source/MessageBus.cpp
    Source/MessageDispatcher.cpp
    Source/MotionEvents.cpp
    Source/ObservingMotionFilter.cpp
    Source/OperationalStatus.cpp
    Source/PtzMovementLease.cpp
    Source/Recording.cpp
    Source/ReolinkClient.cpp
    Source/OnvifPtzClient.cpp
    ../ThirdParty/tinyxml2/tinyxml2.cpp
    Source/SetupConfig.cpp
    Source/SetupServer.cpp
    Source/Shutdown.cpp
    Source/SoundManager.cpp
    Source/SQLite.cpp
    Source/StatusEvents.cpp
    Source/SubStreamWorker.cpp
    Source/TagHelpers.cpp
    Source/TerminalDashboard.cpp
    Source/TimerWorker.cpp
    Source/WatchdogWorker.cpp
    Source/WorkerBase.cpp
)

set(SERVER_HEADERS
    Source/OnvifPtzClient.h
    Source/AudioIntelligenceWorker.h
    Source/AsyncWorker.h
    Source/AuthHelpers.h
    Source/CameraState.h
    Source/CameraWorker.h
    Source/ClipHelpers.h
    Source/ClientHealthHistory.h
    Source/ClientHealthPayload.h
    Source/ClientHealthEndpoint.h
    Source/ConfigPaths.h
    Source/DetectionAssetFiles.h
    Source/DetectionCleanup.h
    Source/ClipReprocessWorker.h
    Source/Common.h
    Source/CrowAuth.h
    Source/CrowListener.h
    Source/Database.h
    Source/FaceRecognitionCache.h
    Source/GlobalContext.h
    Source/ImageProcessorWorker.h
    Source/HttpServerDiagnostics.h
    Source/LongPoll.h
    Source/Message.h
    Source/MessageBus.h
    Source/Messages.h
    Source/ObservingMotionFilter.h
    Source/ReolinkClient.h
    Source/SetupConfig.h
    Source/SetupServer.h
    Source/SettingsMap.h
    Source/SoundManager.h
    Source/SQLite.h
    Source/SubStreamWorker.h
    Source/TagHelpers.h
    Source/TerminalDashboard.h
    Source/OperationalStatus.h
    Source/PtzMovementLease.h
    Source/EventBroadcaster.h
    Source/TimerWorker.h
    Source/WatchdogWorker.h
    Source/Witness.h
    Source/WorkerBase.h
)

add_executable(WitnessServer ${SERVER_SOURCES} ${SERVER_HEADERS})
add_dependencies(WitnessServer NativeBuildIdentity)
target_compile_definitions(WitnessServer PRIVATE WITNESS_NATIVE_CONFIGURATION="$<CONFIG>")

# Crow/Asio is the dominant common parse cost across the HTTP translation
# units. A project-local PCH avoids repeatedly rebuilding that template graph;
# it does not include camera implementation headers or alter their ABI.
if(MSVC)
    target_precompile_headers(WitnessServer PRIVATE "${WITNESS_CROW_ASYNC_INCLUDE}/crow.h")
endif()

target_include_directories(WitnessServer PRIVATE
    "${WITNESS_CROW_ASYNC_INCLUDE}"
    "${CMAKE_BINARY_DIR}/generated"
    Source
    ${CMAKE_SOURCE_DIR}/WitnessCamera/Include
    ${CMAKE_SOURCE_DIR}/ThirdParty/httplib
    ${FFMPEG_INCLUDE_DIRS}
)

target_compile_definitions(WitnessServer PRIVATE
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
    CROW_ENABLE_SSL
)

if(MSVC)
    target_compile_options(WitnessServer PRIVATE /utf-8 /W3)
    target_compile_definitions(WitnessServer PRIVATE _WINDOWS _WIN32 UNICODE _UNICODE)
endif()

if(WIN32)
    target_link_libraries(WitnessServer PRIVATE winmm psapi)
endif()

target_link_directories(WitnessServer PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(WitnessServer PRIVATE
    WitnessCamera
    Crow::Crow
    OpenSSL::SSL
    OpenSSL::Crypto
    ${OpenCV_LIBS}
    ${FFMPEG_LIBRARIES}
    unofficial-sodium::sodium
    unofficial::sqlite3::sqlite3
)

# Focused retention safety test; built explicitly, not on every server build.
# Compile messages/timer declarations without the server's Crow PCH or global
# context imports. This catches accidental restoration of heavyweight coupling.
add_executable(MessageHeaderIsolationTests EXCLUDE_FROM_ALL
    ../Tools/Tests/MessageHeaderIsolationTests.cpp)
target_include_directories(MessageHeaderIsolationTests PRIVATE Source ${OpenCV_INCLUDE_DIRS})
target_link_libraries(MessageHeaderIsolationTests PRIVATE WitnessCamera)
if(MSVC)
    target_compile_options(MessageHeaderIsolationTests PRIVATE /utf-8 /W3)
endif()

add_executable(GlobalContextHeaderIsolationTests EXCLUDE_FROM_ALL
    ../Tools/Tests/GlobalContextHeaderIsolationTests.cpp
    Source/GlobalContext.cpp)
target_include_directories(GlobalContextHeaderIsolationTests PRIVATE Source)
target_link_libraries(GlobalContextHeaderIsolationTests PRIVATE WitnessCamera Crow::Crow unofficial::sqlite3::sqlite3)
if(MSVC)
    target_compile_options(GlobalContextHeaderIsolationTests PRIVATE /utf-8 /W3)
endif()

add_executable(DetectionCleanupTests EXCLUDE_FROM_ALL
    ../Tools/Tests/DetectionCleanupTests.cpp
    Source/DetectionCleanup.cpp
    Source/DetectionAssetFiles.cpp
)
target_include_directories(DetectionCleanupTests PRIVATE Source)
target_link_libraries(DetectionCleanupTests PRIVATE WitnessCommon unofficial::sqlite3::sqlite3)
if(MSVC)
    target_compile_options(DetectionCleanupTests PRIVATE /utf-8 /W3)
endif()

# Explicit, loopback-only real-cookie health integration fixture. Never starts
# camera workers or uses the configured server database; Python owns its fixtures.
add_executable(ClientHealthHttpFixture EXCLUDE_FROM_ALL
    ../Tools/Tests/ClientHealthHttpFixture.cpp
    Source/CrowAuth.cpp Source/SQLite.cpp Source/Common.cpp Source/ClientHealthHistory.cpp
)
target_include_directories(ClientHealthHttpFixture PRIVATE
    Source ${CMAKE_SOURCE_DIR}/WitnessCamera/Include ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(ClientHealthHttpFixture PRIVATE
    WitnessCommon Crow::Crow unofficial::sqlite3::sqlite3
)
if(MSVC)
    target_compile_options(ClientHealthHttpFixture PRIVATE /utf-8 /W3)
    target_compile_definitions(ClientHealthHttpFixture PRIVATE _WINDOWS _WIN32_WINNT=0x0A00)
endif()

# Isolated PTZ safety/query tests: never initialize a camera transport.
find_package(Threads REQUIRED)
add_executable(WorkerShutdownDelayTests EXCLUDE_FROM_ALL
    ../Tools/Tests/WorkerShutdownDelayTests.cpp Source/MessageBus.cpp)
target_include_directories(WorkerShutdownDelayTests PRIVATE Source)
target_link_libraries(WorkerShutdownDelayTests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(WorkerShutdownDelayTests PRIVATE /utf-8 /W3)
endif()
# Exercise the real native thread and timer callback on both platforms.
target_sources(WorkerShutdownDelayTests PRIVATE Source/WorkerBase.cpp Source/TimerWorker.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Worker status contains a timestamp/pointer pair: GCC's 16-byte atomic
    # operations use libatomic on common Linux targets.
    target_link_libraries(WitnessServer PRIVATE atomic)
    target_link_libraries(WorkerShutdownDelayTests PRIVATE atomic)
endif()
add_executable(PtzMovementLeaseTests EXCLUDE_FROM_ALL
    ../Tools/PtzMovementLeaseTests.cpp Source/PtzMovementLease.cpp)
add_executable(PtzRequestExecutorTests EXCLUDE_FROM_ALL
    ../Tools/Tests/PtzRequestExecutorTests.cpp Source/PtzRequestExecutor.cpp)
target_include_directories(PtzRequestExecutorTests PRIVATE Source)
target_link_libraries(PtzRequestExecutorTests PRIVATE Threads::Threads)
add_executable(PtzConfigurationTests EXCLUDE_FROM_ALL
    ../Tools/Tests/PtzConfigurationTests.cpp Source/SQLite.cpp Source/Common.cpp Source/PtzMovementLease.cpp)
target_include_directories(PtzConfigurationTests PRIVATE Source)
target_link_libraries(PtzMovementLeaseTests PRIVATE Threads::Threads)
target_link_libraries(PtzConfigurationTests PRIVATE WitnessCommon unofficial::sqlite3::sqlite3 Threads::Threads)
if(MSVC)
    target_compile_options(PtzMovementLeaseTests PRIVATE /utf-8 /W3)
    target_compile_options(PtzConfigurationTests PRIVATE /utf-8 /W3)
endif()

# Explicit telemetry limit/privacy regressions, with caller-owned temporary DBs.
add_executable(ServerTraceObserverTests EXCLUDE_FROM_ALL
    ../Tools/Tests/ServerTraceObserverTests.cpp Source/HttpServerDiagnostics.cpp Source/SQLite.cpp Source/Common.cpp)
target_include_directories(ServerTraceObserverTests PRIVATE Source)
target_link_libraries(ServerTraceObserverTests PRIVATE WitnessCommon unofficial::sqlite3::sqlite3 Threads::Threads)
if(MSVC)
    target_compile_options(ServerTraceObserverTests PRIVATE /utf-8 /W3)
endif()

add_executable(CpuUsageTests EXCLUDE_FROM_ALL ../Tools/Tests/CpuUsageTests.cpp Source/CpuUsage.cpp)
target_include_directories(CpuUsageTests PRIVATE Source)
target_link_libraries(CpuUsageTests PRIVATE Threads::Threads Crow::Crow)
if(MSVC)
    target_compile_options(CpuUsageTests PRIVATE /utf-8 /W3)
endif()

add_executable(PtzCapabilityProbe EXCLUDE_FROM_ALL
    ../Tools/Tests/PtzCapabilityProbe.cpp Source/ReolinkClient.cpp)
target_include_directories(PtzCapabilityProbe PRIVATE Source ${CMAKE_SOURCE_DIR}/ThirdParty/httplib)
target_link_libraries(PtzCapabilityProbe PRIVATE WitnessCommon Crow::Crow OpenSSL::SSL OpenSSL::Crypto)
if(MSVC)
    target_compile_options(PtzCapabilityProbe PRIVATE /utf-8 /W3)
endif()

add_executable(PtzTransportProbe EXCLUDE_FROM_ALL
    ../Tools/Tests/PtzTransportProbe.cpp Source/ReolinkClient.cpp)
target_include_directories(PtzTransportProbe PRIVATE Source ${CMAKE_SOURCE_DIR}/ThirdParty/httplib)
target_link_libraries(PtzTransportProbe PRIVATE WitnessCommon Crow::Crow OpenSSL::SSL OpenSSL::Crypto)
if(MSVC)
    target_compile_options(PtzTransportProbe PRIVATE /utf-8 /W3)
endif()

# Manual physical movement; requires explicit per-camera confirmation.
add_executable(PtzPhysicalProbe EXCLUDE_FROM_ALL
    ../Tools/Tests/PtzPhysicalProbe.cpp Source/ReolinkClient.cpp)
target_include_directories(PtzPhysicalProbe PRIVATE Source ${CMAKE_SOURCE_DIR}/ThirdParty/httplib)
target_link_libraries(PtzPhysicalProbe PRIVATE WitnessCamera WitnessCommon Crow::Crow OpenSSL::SSL OpenSSL::Crypto)
if(MSVC)
    target_compile_options(PtzPhysicalProbe PRIVATE /utf-8 /W3)
endif()

add_executable(ClientHealthPayloadTests EXCLUDE_FROM_ALL ../Tools/Tests/ClientHealthPayloadTests.cpp)
add_executable(HealthSnapshotCacheTests EXCLUDE_FROM_ALL ../Tools/Tests/HealthSnapshotCacheTests.cpp)
target_include_directories(HealthSnapshotCacheTests PRIVATE Source)
target_link_libraries(HealthSnapshotCacheTests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(HealthSnapshotCacheTests PRIVATE /utf-8 /W3)
endif()
target_include_directories(ClientHealthPayloadTests PRIVATE Source)
target_link_libraries(ClientHealthPayloadTests PRIVATE Crow::Crow)
foreach(health_test ClientHealthHistoryTests ClientHealthEndpointTests)
    add_executable(${health_test} EXCLUDE_FROM_ALL
        ../Tools/Tests/${health_test}.cpp Source/ClientHealthHistory.cpp)
    target_include_directories(${health_test} PRIVATE Source)
    target_link_libraries(${health_test} PRIVATE Crow::Crow unofficial::sqlite3::sqlite3 Threads::Threads)
endforeach()
if(MSVC)
    foreach(health_test ClientHealthPayloadTests ClientHealthHistoryTests ClientHealthEndpointTests)
        target_compile_options(${health_test} PRIVATE /utf-8 /W3)
    endforeach()
endif()

# Pure path validation and opt-in data-root isolation.
add_executable(DvrConfigurationTests EXCLUDE_FROM_ALL ../Tools/Tests/DvrConfigurationTests.cpp)
target_include_directories(DvrConfigurationTests PRIVATE Source)

add_executable(RecentCaptureJobsTests EXCLUDE_FROM_ALL
    ../Tools/Tests/RecentCaptureJobsTests.cpp Source/RecentCaptureJobs.cpp)
target_include_directories(RecentCaptureJobsTests PRIVATE Source)
target_link_libraries(RecentCaptureJobsTests PRIVATE WitnessCamera Threads::Threads)

add_executable(ConfigPathsTests EXCLUDE_FROM_ALL
    ../Tools/Tests/ConfigPathsTests.cpp Source/ConfigPaths.cpp)
target_include_directories(ConfigPathsTests PRIVATE Source)
if(MSVC)
    target_compile_options(ConfigPathsTests PRIVATE /utf-8 /W3)
endif()

# Copy models directory next to the executable
add_custom_command(TARGET WitnessServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:WitnessServer>/models"
    COMMENT "Copying models directory"
)

# Copy OpenSSL CLI tool to output (used by Installer/Setup-TLS.ps1 for self-signed certs)
find_program(OPENSSL_EXECUTABLE openssl
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/openssl"
    NO_DEFAULT_PATH
)
if(OPENSSL_EXECUTABLE)
    cmake_path(GET OPENSSL_EXECUTABLE PARENT_PATH OPENSSL_TOOL_DIR)
    add_custom_command(TARGET WitnessServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_EXECUTABLE}"
            "$<TARGET_FILE_DIR:WitnessServer>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_TOOL_DIR}/openssl.cnf"
            "$<TARGET_FILE_DIR:WitnessServer>/"
        COMMENT "Copying OpenSSL CLI tool and config"
    )
endif()

# Copy ONNX Runtime CUDA provider DLLs (loaded dynamically, not caught by applocal)
set(ONNX_PROVIDERS_SHARED "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/onnxruntime_providers_shared.dll")
set(ONNX_PROVIDERS_CUDA "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/onnxruntime_providers_cuda.dll")
if(EXISTS "${ONNX_PROVIDERS_SHARED}" AND EXISTS "${ONNX_PROVIDERS_CUDA}")
    add_custom_command(TARGET WitnessServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNX_PROVIDERS_SHARED}"
            "$<TARGET_FILE_DIR:WitnessServer>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNX_PROVIDERS_CUDA}"
            "$<TARGET_FILE_DIR:WitnessServer>/"
        COMMENT "Copying ONNX Runtime CUDA provider DLLs"
    )
endif()
