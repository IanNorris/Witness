cmake_minimum_required(VERSION 3.20)

# Auto-detect vcpkg and set up the toolchain before project()
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "" AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    # Already set and valid — use it
else()
    set(_VCPKG_TOOLCHAIN "")

    # 1. Check VCPKG_ROOT environment variable
    if(NOT _VCPKG_TOOLCHAIN AND DEFINED ENV{VCPKG_ROOT})
        set(_CANDIDATE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        if(EXISTS "${_CANDIDATE}")
            set(_VCPKG_TOOLCHAIN "${_CANDIDATE}")
        endif()
    endif()

    # 2. Try vcpkg hint file (written by 'vcpkg integrate install')
    if(NOT _VCPKG_TOOLCHAIN AND DEFINED ENV{LOCALAPPDATA})
        set(_VCPKG_HINT_FILE "$ENV{LOCALAPPDATA}/vcpkg/vcpkg.path.txt")
        if(EXISTS "${_VCPKG_HINT_FILE}")
            file(READ "${_VCPKG_HINT_FILE}" _VCPKG_ROOT)
            string(STRIP "${_VCPKG_ROOT}" _VCPKG_ROOT)
            set(_CANDIDATE "${_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
            if(EXISTS "${_CANDIDATE}")
                set(_VCPKG_TOOLCHAIN "${_CANDIDATE}")
                message(STATUS "Found vcpkg via hint file: ${_VCPKG_ROOT}")
            endif()
        endif()
    endif()

    if(_VCPKG_TOOLCHAIN)
        set(CMAKE_TOOLCHAIN_FILE "${_VCPKG_TOOLCHAIN}" CACHE STRING "" FORCE)
    else()
        message(FATAL_ERROR
            "Cannot find vcpkg. Set VCPKG_ROOT environment variable, "
            "run 'vcpkg integrate install', or pass "
            "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()

project(Witness LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate PDBs even in Release builds (MSVC)
if(MSVC)
    add_compile_options(/Zi)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
endif()

# Put all binaries in a single output directory so DLLs are found at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(WitnessCommon)
add_subdirectory(WitnessCamera)
add_subdirectory(WitnessServer)

# Build the .NET 8 Installer as a self-contained exe (DEPRECATED — use built-in /setup wizard)
# Build with: cmake --build build --target Installer
find_program(DOTNET_EXECUTABLE dotnet)
if(DOTNET_EXECUTABLE AND WIN32)
    add_custom_target(Installer
        COMMAND ${DOTNET_EXECUTABLE} publish
            "${CMAKE_SOURCE_DIR}/Installer/Installer.csproj"
            -c Release
            -o "${CMAKE_BINARY_DIR}/bin"
            --no-restore
        COMMENT "Publishing Installer (self-contained) [DEPRECATED]"
    )

    # Restore NuGet packages at configure time so --no-restore works at build time
    execute_process(
        COMMAND ${DOTNET_EXECUTABLE} restore "${CMAKE_SOURCE_DIR}/Installer/Installer.csproj"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

# Copy Setup-TLS.ps1 to output
configure_file(
    "${CMAKE_SOURCE_DIR}/Setup/Setup-TLS.ps1"
    "${CMAKE_BINARY_DIR}/bin/Setup-TLS.ps1"
    COPYONLY
)

# Build Vue 3 web UI and copy to Web/witness2/
find_program(NPM_EXECUTABLE npm)
if(NPM_EXECUTABLE)
    add_custom_target(WebUI
        COMMAND ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/web-ui"
        COMMENT "Building Vue 3 web UI"
    )
    add_dependencies(WitnessServer WebUI)
endif()

# Copy ONNX models to output (if any exist)
file(GLOB ONNX_MODELS "${CMAKE_SOURCE_DIR}/models/*.onnx")
foreach(MODEL_FILE ${ONNX_MODELS})
    get_filename_component(MODEL_NAME "${MODEL_FILE}" NAME)
    configure_file("${MODEL_FILE}" "${CMAKE_BINARY_DIR}/bin/models/${MODEL_NAME}" COPYONLY)
endforeach()
