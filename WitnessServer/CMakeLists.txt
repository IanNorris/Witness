find_package(cpprestsdk CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

set(SERVER_SOURCES
    Source/Android/AndroidNotify.cpp
    Source/AsyncWorker.cpp
    Source/Azure/AzureEndpoint.cpp
    Source/Azure/AzureVisionAnalysisEndpointFilter.cpp
    Source/CrowAuth.cpp
    Source/CrowListener.cpp
    Source/CameraWorker.cpp
    Source/Commands/Authenticate.cpp
    Source/Commands/Camera.cpp
    Source/Commands/Clip.cpp
    Source/Commands/Debug.cpp
    Source/Commands/Group.cpp
    Source/Commands/Static.cpp
    Source/Commands/Stream.cpp
    Source/Common.cpp
    Source/Database.cpp
    Source/ImageProcessorWorker.cpp
    Source/Init.cpp
    Source/JsonHelpers.cpp
    Source/Listener.cpp
    Source/Main.cpp
    Source/MessageBus.cpp
    Source/MessageDispatcher.cpp
    Source/MotionEvents.cpp
    Source/ObservingMotionFilter.cpp
    Source/Recording.cpp
    Source/Shutdown.cpp
    Source/SQLite.cpp
    Source/StatusEvents.cpp
    Source/TimerWorker.cpp
    Source/WatchdogWorker.cpp
    Source/WorkerBase.cpp
)

set(SERVER_HEADERS
    Source/Android/AndroidNotify.h
    Source/AsyncWorker.h
    Source/Azure/AzureEndpoint.h
    Source/Azure/AzureVisionAnalysisEndpointFilter.h
    Source/CameraState.h
    Source/CameraWorker.h
    Source/Commands/Authenticate.h
    Source/Commands/Camera.h
    Source/Commands/Clip.h
    Source/Commands/Debug.h
    Source/Commands/Group.h
    Source/Commands/Static.h
    Source/Commands/Stream.h
    Source/Common.h
    Source/CrowAuth.h
    Source/CrowListener.h
    Source/Database.h
    Source/GlobalContext.h
    Source/ImageProcessorWorker.h
    Source/Listener.h
    Source/ListenerCommand.h
    Source/LongPoll.h
    Source/Message.h
    Source/MessageBus.h
    Source/Messages.h
    Source/ObservingMotionFilter.h
    Source/SettingsMap.h
    Source/SQLite.h
    Source/TimerWorker.h
    Source/WatchdogWorker.h
    Source/Witness.h
    Source/WorkerBase.h
)

add_executable(WitnessServer ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(WitnessServer PRIVATE
    Source
    ${CMAKE_SOURCE_DIR}/WitnessCamera/Include
)

target_compile_definitions(WitnessServer PRIVATE
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
)

if(MSVC)
    target_compile_options(WitnessServer PRIVATE /utf-8 /W3)
    target_compile_definitions(WitnessServer PRIVATE _WINDOWS _WIN32 UNICODE _UNICODE)
endif()

if(WIN32)
    target_link_libraries(WitnessServer PRIVATE winmm)
endif()

target_link_libraries(WitnessServer PRIVATE
    WitnessCamera
    cpprestsdk::cpprest
    Crow::Crow
    ${OpenCV_LIBS}
    unofficial-sodium::sodium
    unofficial::sqlite3::sqlite3
)

# Copy the Web directory next to the executable
add_custom_command(TARGET WitnessServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_SOURCE_DIR}/WitnessServer/Web"
        "$<TARGET_FILE_DIR:WitnessServer>/Web"
    COMMENT "Symlinking Web directory"
)
