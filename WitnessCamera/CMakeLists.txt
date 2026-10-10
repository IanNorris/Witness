find_package(FFMPEG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect video)
find_package(lz4 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# The vcpkg 1.23.2 build registers ONNX schemas twice at runtime because its
# ONNX dependency is not built with ONNX_DISABLE_STATIC_REGISTRATION. Use the
# official Microsoft CPU binary instead. A local extraction can be supplied for
# offline builds with -DWITNESS_ONNXRUNTIME_ROOT=<path>.
set(WITNESS_ONNXRUNTIME_VERSION "1.23.2" CACHE STRING "ONNX Runtime version")
set(WITNESS_ONNXRUNTIME_ROOT "" CACHE PATH "Path to an extracted official ONNX Runtime package")

if(NOT WITNESS_ONNXRUNTIME_ROOT)
    include(FetchContent)
    FetchContent_Declare(onnxruntime_prebuilt
        URL "https://github.com/microsoft/onnxruntime/releases/download/v${WITNESS_ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${WITNESS_ONNXRUNTIME_VERSION}.zip"
        URL_HASH "SHA256=0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(onnxruntime_prebuilt)
    set(WITNESS_ONNXRUNTIME_ROOT "${onnxruntime_prebuilt_SOURCE_DIR}")
endif()

set(WITNESS_ONNXRUNTIME_LIBRARY "${WITNESS_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
set(WITNESS_ONNXRUNTIME_DLL "${WITNESS_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
set(WITNESS_ONNXRUNTIME_INCLUDE_DIR "${WITNESS_ONNXRUNTIME_ROOT}/include")

if(NOT EXISTS "${WITNESS_ONNXRUNTIME_LIBRARY}" OR
   NOT EXISTS "${WITNESS_ONNXRUNTIME_DLL}" OR
   NOT EXISTS "${WITNESS_ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "Invalid official ONNX Runtime package at ${WITNESS_ONNXRUNTIME_ROOT}")
endif()

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_IMPLIB "${WITNESS_ONNXRUNTIME_LIBRARY}"
    IMPORTED_LOCATION "${WITNESS_ONNXRUNTIME_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${WITNESS_ONNXRUNTIME_INCLUDE_DIR}"
)

set(WITNESS_ONNX_CUDA_AVAILABLE 0)

set(CAMERA_SOURCES
    Source/NativeBuildIdentity.cpp
    Source/AudioClassifier.cpp
    Source/ContinuousOutputStream.cpp
    Source/HistoricalClip.cpp
    Source/DebugBind.cpp
    Source/FaceDetectionFilter.cpp
    Source/FaceEmbeddingModel.cpp
    Source/FilterFrame.cpp
    Source/InMemoryIOContext.cpp
    Source/LiveOutputStream.cpp
    Source/MotionVectorFilter.cpp
    Source/ONNXDetectionFilter.cpp
    Source/PacketCapture.cpp
    Source/PersonRecognitionFilter.cpp
    Source/ReolinkBaichuanClient.cpp
    Source/ReolinkUnofficialFilter.cpp
    Source/FFMPEG/Frame.cpp
    Source/ImageProcessingJobQueue.cpp
    Source/InputStream.cpp
    Source/MotionFilter.cpp
    Source/OutputStream.cpp
    Source/Stream.cpp
    Source/StreamManager.cpp
)

set(CAMERA_HEADERS
    Include/AudioClassifier.h
    Include/ContinuousOutputStream.h
    Include/HistoricalClip.h
    Include/CadenceDeliveryEvidence.h
    Include/DebugBind.h
    Include/Export.h
    Include/FaceDetectionFilter.h
    Include/FaceEmbeddingModel.h
    Include/ImageProcessingJob.h
    Include/InMemoryIOContext.h
    Include/InputStream.h
    Include/LiveOutputStream.h
    Include/MotionFilter.h
    Include/MotionVectorFilter.h
    Include/ONNXDetectionFilter.h
    Include/OutputStream.h
    Include/PersonRecognitionFilter.h
    Include/Pimpl.h
    Include/RecordFilter.h
    Include/RecordFilterBase.h
    Include/ReolinkBaichuanClient.h
    Include/ReolinkUnofficialFilter.h
    Include/SourceStats.h
    Include/Stream.h
    Source/FFMPEG/Common.h
    Source/FilterData.h
    Source/ImageProcessingData.h
    Source/StreamData.h
    Source/LiveOutputStreamData.h
    Source/StreamManager.h
)

add_library(WitnessCamera SHARED ${CAMERA_SOURCES} ${CAMERA_HEADERS})
add_dependencies(WitnessCamera NativeBuildIdentity)
target_include_directories(WitnessCamera PRIVATE "${CMAKE_BINARY_DIR}/generated")

target_include_directories(WitnessCamera
    PUBLIC  Include
    PRIVATE Source
            ${FFMPEG_INCLUDE_DIRS}
)

target_compile_definitions(WitnessCamera PRIVATE
    WITNESS_NATIVE_CONFIGURATION="$<CONFIG>"
    WITNESSCAMERA_EXPORTS
    WITNESS_ONNX_CUDA_AVAILABLE=${WITNESS_ONNX_CUDA_AVAILABLE}
)

if(MSVC)
    target_compile_options(WitnessCamera PRIVATE /utf-8 /W3)
    target_compile_definitions(WitnessCamera PRIVATE _WINDOWS _USRDLL)
endif()

target_link_directories(WitnessCamera PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(WitnessCamera
    PRIVATE
        ${FFMPEG_LIBRARIES}
        ${OpenCV_LIBS}
        onnxruntime::onnxruntime
        lz4::lz4
        OpenSSL::Crypto
        OpenSSL::SSL
    PUBLIC
        WitnessCommon
)

add_custom_command(TARGET WitnessCamera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WITNESS_ONNXRUNTIME_DLL}"
        "$<TARGET_FILE_DIR:WitnessCamera>/"
    COMMENT "Copying official CPU-only ONNX Runtime"
)

add_executable(PacketCaptureTests EXCLUDE_FROM_ALL
    ../Tools/Tests/PacketCaptureTests.cpp
    Source/PacketCapture.cpp
)
target_include_directories(PacketCaptureTests PRIVATE Source)
if(MSVC)
    target_compile_options(PacketCaptureTests PRIVATE /utf-8 /W3)
endif()

add_executable(TimestampRegressionGuardTests EXCLUDE_FROM_ALL
    ../Tools/Tests/TimestampRegressionGuardTests.cpp
)
target_include_directories(TimestampRegressionGuardTests PRIVATE Source Include)
if(MSVC)
    target_compile_options(TimestampRegressionGuardTests PRIVATE /utf-8 /W3)
endif()

# Manual, opt-in real-camera verification; never run as an unattended test.
add_executable(LivePreviewProbe EXCLUDE_FROM_ALL ../Tools/Tests/LivePreviewProbe.cpp)
target_include_directories(LivePreviewProbe PRIVATE Include ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_link_directories(LivePreviewProbe PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(LivePreviewProbe PRIVATE WitnessCamera ${FFMPEG_LIBRARIES})

add_executable(InputPacingProbe EXCLUDE_FROM_ALL ../Tools/Tests/InputPacingProbe.cpp)
target_include_directories(InputPacingProbe PRIVATE Include ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(InputPacingProbe PRIVATE WitnessCamera)

# Synthetic loopback peer only; exercises exported Camera DLL ownership.
add_executable(BaichuanCommandReplyTests EXCLUDE_FROM_ALL ../Tools/Tests/BaichuanCommandReplyTests.cpp)
target_include_directories(BaichuanCommandReplyTests PRIVATE Source)
if(MSVC)
    target_compile_options(BaichuanCommandReplyTests PRIVATE /utf-8 /W3)
endif()

add_executable(ProcessingLatencyWindowTests EXCLUDE_FROM_ALL ../Tools/Tests/ProcessingLatencyWindowTests.cpp)
target_include_directories(ProcessingLatencyWindowTests PRIVATE Source Include)
if(MSVC)
    target_compile_options(ProcessingLatencyWindowTests PRIVATE /utf-8 /W3)
endif()

add_executable(RecentWrittenVideoPacketsTests EXCLUDE_FROM_ALL ../Tools/Tests/RecentWrittenVideoPacketsTests.cpp)
target_include_directories(RecentWrittenVideoPacketsTests PRIVATE Source)
if(MSVC)
    target_compile_options(RecentWrittenVideoPacketsTests PRIVATE /utf-8 /W3)
endif()

add_executable(BaichuanLifetimeTests EXCLUDE_FROM_ALL ../Tools/Tests/BaichuanLifetimeTests.cpp)
add_executable(BaichuanPtzProbe EXCLUDE_FROM_ALL ../Tools/Tests/BaichuanPtzProbe.cpp)
target_link_libraries(BaichuanPtzProbe PRIVATE WitnessCamera)
target_link_libraries(BaichuanLifetimeTests PRIVATE WitnessCamera)
if(WIN32)
    target_link_libraries(BaichuanLifetimeTests PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(BaichuanLifetimeTests PRIVATE /utf-8 /W3)
endif()

# Offline fixtures only. Internal state access belongs to these test consumers,
# never to the exported Camera DLL header.
foreach(test_name LiveTimestampRecoveryTests LiveCadencePauseTests LiveStreamOwnershipTests LiveDuplicateHistoryTests MediaTraceObserverTests LiveAacRemuxTests RecordingFinalizationTests ContinuousOpenSegmentTests HistoricalClipTests)
    add_executable(${test_name} EXCLUDE_FROM_ALL ../Tools/Tests/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE Source ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
    target_link_directories(${test_name} PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(${test_name} PRIVATE WitnessCamera ${FFMPEG_LIBRARIES})
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /utf-8 /W3)
    endif()
endforeach()

add_executable(ImageQueueShutdownTests EXCLUDE_FROM_ALL ../Tools/Tests/ImageQueueShutdownTests.cpp)
target_include_directories(ImageQueueShutdownTests PRIVATE ${FFMPEG_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(ImageQueueShutdownTests PRIVATE WitnessCamera)
if(MSVC)
    target_compile_options(ImageQueueShutdownTests PRIVATE /utf-8 /W3)
endif()

# Compile the wrapper directly so its ownership contract can be tested without
# rebuilding the server or running any camera/database/network services.
add_executable(FrameOwnershipTests EXCLUDE_FROM_ALL
    ../Tools/Tests/FrameOwnershipTests.cpp Source/FFMPEG/Frame.cpp)
target_compile_definitions(FrameOwnershipTests PRIVATE WITNESSCAMERA_EXPORTS)
target_include_directories(FrameOwnershipTests PRIVATE Include ${FFMPEG_INCLUDE_DIRS})
target_link_directories(FrameOwnershipTests PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(FrameOwnershipTests PRIVATE ${FFMPEG_LIBRARIES})
if(MSVC)
    target_compile_options(FrameOwnershipTests PRIVATE /utf-8 /W3)
endif()
