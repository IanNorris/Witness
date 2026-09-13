cmake_minimum_required(VERSION 3.20)

# vcpkg does not yet auto-discover VS 2026 Insiders on this machine. Pin both
# the project and dependency builds to it unless the caller explicitly selects
# another installation with VCPKG_VISUAL_STUDIO_PATH.
if(CMAKE_HOST_WIN32)
    if(NOT DEFINED ENV{VCPKG_VISUAL_STUDIO_PATH} OR "$ENV{VCPKG_VISUAL_STUDIO_PATH}" STREQUAL "")
        set(ENV{VCPKG_VISUAL_STUDIO_PATH} "$ENV{ProgramFiles}/Microsoft Visual Studio/18/Insiders")
    endif()

    if(NOT EXISTS "$ENV{VCPKG_VISUAL_STUDIO_PATH}")
        message(FATAL_ERROR
            "The selected Visual Studio installation was not found at "
            "$ENV{VCPKG_VISUAL_STUDIO_PATH}. "
            "Install it or explicitly set VCPKG_VISUAL_STUDIO_PATH to the Visual Studio "
            "installation that should build both Witness and its vcpkg dependencies.")
    endif()
endif()

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

# A Visual Studio or Ninja environment can still select a different cl.exe from
# the one vcpkg was pinned to. Reject that combination before it can invalidate
# or produce ABI-incompatible dependencies.
if(MSVC AND DEFINED ENV{VCPKG_VISUAL_STUDIO_PATH})
    file(TO_CMAKE_PATH "$ENV{VCPKG_VISUAL_STUDIO_PATH}" _WITNESS_VS_PATH)
    file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" _WITNESS_CXX_COMPILER)
    string(REGEX REPLACE "/+$" "" _WITNESS_VS_PATH "${_WITNESS_VS_PATH}")
    string(TOLOWER "${_WITNESS_VS_PATH}" _WITNESS_VS_PATH_LOWER)
    string(TOLOWER "${_WITNESS_CXX_COMPILER}" _WITNESS_CXX_COMPILER_LOWER)
    string(FIND "${_WITNESS_CXX_COMPILER_LOWER}" "${_WITNESS_VS_PATH_LOWER}/" _WITNESS_VS_PATH_POS)
    if(NOT _WITNESS_VS_PATH_POS EQUAL 0)
        message(FATAL_ERROR
            "CMake selected ${CMAKE_CXX_COMPILER}, but vcpkg is pinned to "
            "$ENV{VCPKG_VISUAL_STUDIO_PATH}. Start CMake from that Visual Studio "
            "environment, or explicitly override VCPKG_VISUAL_STUDIO_PATH.")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate PDBs even in Release builds (MSVC)
if(MSVC)
    add_compile_options(/Z7)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
endif()

# Put all binaries in a single output directory so DLLs are found at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(WitnessCommon)
add_subdirectory(WitnessCamera)
add_subdirectory(WitnessServer)
add_subdirectory(Tools/BaichuanTest)

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

# Copy ONNX models to output at build time
add_custom_target(CopyModels ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/models"
    COMMENT "Copying ONNX models..."
)
file(GLOB ONNX_MODELS "${CMAKE_SOURCE_DIR}/models/*.onnx")
foreach(MODEL_FILE ${ONNX_MODELS})
    get_filename_component(MODEL_NAME "${MODEL_FILE}" NAME)
    add_custom_command(TARGET CopyModels POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL_FILE}" "${CMAKE_BINARY_DIR}/bin/models/${MODEL_NAME}"
    )
endforeach()

# Copy notification sounds to output
file(GLOB NOTIFICATION_SOUNDS "${CMAKE_SOURCE_DIR}/Assets/NotificationSounds/*.wav")
foreach(SOUND_FILE ${NOTIFICATION_SOUNDS})
    get_filename_component(SOUND_NAME "${SOUND_FILE}" NAME)
    configure_file("${SOUND_FILE}" "${CMAKE_BINARY_DIR}/bin/NotificationSounds/${SOUND_NAME}" COPYONLY)
endforeach()
