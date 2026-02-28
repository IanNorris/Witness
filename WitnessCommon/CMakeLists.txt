add_library(WitnessCommon SHARED
    Source/Log.cpp
)

target_include_directories(WitnessCommon PUBLIC Include)
target_compile_definitions(WitnessCommon PRIVATE COMMON_EXPORTS)

if(MSVC)
    target_compile_definitions(WitnessCommon PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
