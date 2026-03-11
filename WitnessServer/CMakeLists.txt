find_package(Crow CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

set(SERVER_SOURCES
    Source/AsyncWorker.cpp
    Source/AuthHelpers.cpp
    Source/ClipHelpers.cpp
    Source/ClipReprocessWorker.cpp
    Source/CrowAuth.cpp
    Source/CrowListener.cpp
    Source/CrowRoutes_Auth.cpp
    Source/CrowRoutes_Camera.cpp
    Source/CrowRoutes_Clip.cpp
    Source/CrowRoutes_Debug.cpp
    Source/CrowRoutes_Face.cpp
    Source/CrowRoutes_FaceRecognition.cpp
    Source/CrowRoutes_DVR.cpp
    Source/CrowRoutes_Group.cpp
    Source/CrowRoutes_Action.cpp
    Source/CrowRoutes_Setup.cpp
    Source/CrowRoutes_Stream.cpp
    Source/CrowRoutes_Tag.cpp
    Source/CameraWorker.cpp
    Source/Common.cpp
    Source/Database.cpp
    Source/FaceRecognitionCache.cpp
    Source/ImageProcessorWorker.cpp
    Source/Init.cpp
    Source/JsonHelpers.cpp
    Source/Main.cpp
    Source/MessageBus.cpp
    Source/MessageDispatcher.cpp
    Source/MotionEvents.cpp
    Source/ObservingMotionFilter.cpp
    Source/Recording.cpp
    Source/SetupConfig.cpp
    Source/SetupServer.cpp
    Source/Shutdown.cpp
    Source/SQLite.cpp
    Source/StatusEvents.cpp
    Source/TagHelpers.cpp
    Source/TimerWorker.cpp
    Source/WatchdogWorker.cpp
    Source/WorkerBase.cpp
)

set(SERVER_HEADERS
    Source/AsyncWorker.h
    Source/AuthHelpers.h
    Source/CameraState.h
    Source/CameraWorker.h
    Source/ClipHelpers.h
    Source/ClipReprocessWorker.h
    Source/Common.h
    Source/CrowAuth.h
    Source/CrowListener.h
    Source/Database.h
    Source/FaceRecognitionCache.h
    Source/GlobalContext.h
    Source/ImageProcessorWorker.h
    Source/LongPoll.h
    Source/Message.h
    Source/MessageBus.h
    Source/Messages.h
    Source/ObservingMotionFilter.h
    Source/SetupConfig.h
    Source/SetupServer.h
    Source/SettingsMap.h
    Source/SQLite.h
    Source/TagHelpers.h
    Source/EventBroadcaster.h
    Source/TimerWorker.h
    Source/WatchdogWorker.h
    Source/Witness.h
    Source/WorkerBase.h
)

add_executable(WitnessServer ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(WitnessServer PRIVATE
    Source
    ${CMAKE_SOURCE_DIR}/WitnessCamera/Include
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
    target_link_libraries(WitnessServer PRIVATE winmm)
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

# Copy the Web directory next to the executable
add_custom_command(TARGET WitnessServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_SOURCE_DIR}/WitnessServer/Web"
        "$<TARGET_FILE_DIR:WitnessServer>/Web"
    COMMENT "Symlinking Web directory"
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
