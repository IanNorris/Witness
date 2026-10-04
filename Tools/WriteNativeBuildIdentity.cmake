# Runs at build time, independently of the web bundle and Git commit timing.
# Write only on content changes so web-only builds do not relink native code.
file(GLOB_RECURSE inputs
    "${SOURCE_ROOT}/WitnessCommon/*.cpp" "${SOURCE_ROOT}/WitnessCommon/*.h"
    "${SOURCE_ROOT}/WitnessCamera/*.cpp" "${SOURCE_ROOT}/WitnessCamera/*.h"
    "${SOURCE_ROOT}/WitnessServer/Source/*.cpp" "${SOURCE_ROOT}/WitnessServer/Source/*.h")
list(APPEND inputs "${SOURCE_ROOT}/CMakeLists.txt"
    "${SOURCE_ROOT}/WitnessCommon/CMakeLists.txt"
    "${SOURCE_ROOT}/WitnessCamera/CMakeLists.txt"
    "${SOURCE_ROOT}/WitnessServer/CMakeLists.txt"
    "${SOURCE_ROOT}/Tools/WitnessDependencyPolicy.cmake" "${CMAKE_CURRENT_LIST_FILE}")
foreach(manifest vcpkg.json vcpkg-configuration.json CMakePresets.json)
    if(EXISTS "${SOURCE_ROOT}/${manifest}")
        list(APPEND inputs "${SOURCE_ROOT}/${manifest}")
    endif()
endforeach()
list(SORT inputs)
set(identity "${TOOLCHAIN}")
foreach(input IN LISTS inputs)
    file(SHA256 "${input}" digest)
    file(RELATIVE_PATH relative "${SOURCE_ROOT}" "${input}")
    string(APPEND identity "\n${relative}:${digest}")
endforeach()
string(SHA256 digest "${identity}")
string(SUBSTRING "${digest}" 0 12 short_digest)
set(content "#pragma once\n#define WITNESS_NATIVE_BUILD_ID \"${short_digest}\"\n")
if(EXISTS "${OUTPUT_HEADER}")
    file(READ "${OUTPUT_HEADER}" previous)
else()
    set(previous "")
endif()
if(NOT previous STREQUAL content)
    get_filename_component(directory "${OUTPUT_HEADER}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${OUTPUT_HEADER}" "${content}")
endif()
