find_package(FFMPEG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect video)

# ONNX Runtime (onnxruntime-gpu vcpkg port installs raw headers/libs, no CMake config)
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h)
find_library(ONNXRUNTIME_LIB onnxruntime)

set(CAMERA_SOURCES
    Source/ContinuousOutputStream.cpp
    Source/DebugBind.cpp
    Source/FaceDetectionFilter.cpp
    Source/FilterFrame.cpp
    Source/InMemoryIOContext.cpp
    Source/LiveOutputStream.cpp
    Source/MotionVectorFilter.cpp
    Source/ONNXDetectionFilter.cpp
    Source/PersonRecognitionFilter.cpp
    Source/FFMPEG/Frame.cpp
    Source/ImageProcessingJobQueue.cpp
    Source/InputStream.cpp
    Source/MotionFilter.cpp
    Source/OutputStream.cpp
    Source/Stream.cpp
    Source/StreamManager.cpp
)

set(CAMERA_HEADERS
    Include/ContinuousOutputStream.h
    Include/DebugBind.h
    Include/Export.h
    Include/FaceDetectionFilter.h
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
    Include/SourceStats.h
    Include/Stream.h
    Source/FFMPEG/Common.h
    Source/FilterData.h
    Source/ImageProcessingData.h
    Source/StreamData.h
    Source/StreamManager.h
)

add_library(WitnessCamera SHARED ${CAMERA_SOURCES} ${CAMERA_HEADERS})

target_include_directories(WitnessCamera
    PUBLIC  Include
    PRIVATE Source
            ${FFMPEG_INCLUDE_DIRS}
            ${ONNXRUNTIME_INCLUDE_DIR}
)

target_compile_definitions(WitnessCamera PRIVATE WITNESSCAMERA_EXPORTS)

if(MSVC)
    target_compile_options(WitnessCamera PRIVATE /utf-8 /W3)
    target_compile_definitions(WitnessCamera PRIVATE _WINDOWS _USRDLL)
endif()

target_link_directories(WitnessCamera PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(WitnessCamera
    PRIVATE
        ${FFMPEG_LIBRARIES}
        ${OpenCV_LIBS}
        ${ONNXRUNTIME_LIB}
    PUBLIC
        WitnessCommon
)
