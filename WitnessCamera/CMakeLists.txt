find_package(FFMPEG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect video)

set(CAMERA_SOURCES
    Source/DebugBind.cpp
    Source/FilterFrame.cpp
    Source/InMemoryIOContext.cpp
    Source/LiveOutputStream.cpp
    Source/MotionVectorFilter.cpp
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
    Include/DebugBind.h
    Include/Export.h
    Include/ImageProcessingJob.h
    Include/InMemoryIOContext.h
    Include/InputStream.h
    Include/LiveOutputStream.h
    Include/MotionFilter.h
    Include/MotionVectorFilter.h
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
)
