cmake_minimum_required(VERSION 3.20)

option(WITNESS_MANAGED_DEPENDENCIES
    "Require explicitly bootstrapped, fingerprinted vcpkg dependencies" OFF)
option(WITNESS_BOOTSTRAP_DEPENDENCIES
    "Allow vcpkg to install/update the managed dependency environment" OFF)
option(WITNESS_PROFILE_BUILD
    "Enable verbose MSVC frontend/backend timing diagnostics" OFF)

# Managed builds never mutate dependencies as a side effect of routine IDE or
# command-line configuration. The bootstrap script opts into installation.
if(WITNESS_MANAGED_DEPENDENCIES)
    set(VCPKG_MANIFEST_INSTALL ${WITNESS_BOOTSTRAP_DEPENDENCIES}
        CACHE BOOL "Install vcpkg manifest dependencies" FORCE)
endif()

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

if(WITNESS_MANAGED_DEPENDENCIES)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        message(FATAL_ERROR "Managed dependencies require VCPKG_TARGET_TRIPLET.")
    endif()

    file(SHA256 "${CMAKE_SOURCE_DIR}/vcpkg.json" _WITNESS_MANIFEST_SHA)
    file(SHA256 "${CMAKE_SOURCE_DIR}/vcpkg-configuration.json" _WITNESS_REGISTRY_SHA)
    file(SHA256 "${CMAKE_SOURCE_DIR}/build-environment.json" _WITNESS_ENVIRONMENT_SHA)
    file(READ "${CMAKE_SOURCE_DIR}/build-environment.json" _WITNESS_ENVIRONMENT_JSON)
    string(JSON _WITNESS_EXPECTED_VCPKG_COMMIT GET
        "${_WITNESS_ENVIRONMENT_JSON}" vcpkgCommit)
    string(JSON _WITNESS_EXPECTED_MSVC_FAMILY GET
        "${_WITNESS_ENVIRONMENT_JSON}" msvcCompilerFamily)
    set(_WITNESS_TRIPLET_FILE
        "${CMAKE_SOURCE_DIR}/triplets/${VCPKG_TARGET_TRIPLET}.cmake")
    if(NOT EXISTS "${_WITNESS_TRIPLET_FILE}")
        message(FATAL_ERROR
            "Managed dependency triplet not found: ${_WITNESS_TRIPLET_FILE}")
    endif()
    file(SHA256 "${_WITNESS_TRIPLET_FILE}" _WITNESS_TRIPLET_SHA)

    get_filename_component(_WITNESS_VCPKG_BUILDSYSTEM_DIR
        "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
    get_filename_component(_WITNESS_VCPKG_SCRIPTS_DIR
        "${_WITNESS_VCPKG_BUILDSYSTEM_DIR}" DIRECTORY)
    get_filename_component(_WITNESS_VCPKG_ROOT
        "${_WITNESS_VCPKG_SCRIPTS_DIR}" DIRECTORY)
    if(WIN32)
        set(_WITNESS_VCPKG_EXE "${_WITNESS_VCPKG_ROOT}/vcpkg.exe")
    else()
        set(_WITNESS_VCPKG_EXE "${_WITNESS_VCPKG_ROOT}/vcpkg")
    endif()
    if(NOT EXISTS "${_WITNESS_VCPKG_EXE}")
        message(FATAL_ERROR "Cannot fingerprint vcpkg at ${_WITNESS_VCPKG_EXE}")
    endif()
    execute_process(
        COMMAND "${_WITNESS_VCPKG_EXE}" version
        OUTPUT_VARIABLE _WITNESS_VCPKG_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _WITNESS_VCPKG_VERSION_RESULT)
    if(NOT _WITNESS_VCPKG_VERSION_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not query the selected vcpkg version.")
    endif()
    string(FIND "${_WITNESS_VCPKG_VERSION}"
        "${_WITNESS_EXPECTED_VCPKG_COMMIT}" _WITNESS_VCPKG_COMMIT_POSITION)
    if(_WITNESS_VCPKG_COMMIT_POSITION EQUAL -1)
        message(FATAL_ERROR
            "The selected vcpkg does not match pinned commit "
            "${_WITNESS_EXPECTED_VCPKG_COMMIT}. Run "
            "scripts/bootstrap-vcpkg.ps1 or explicitly select a matching VCPKG_ROOT.")
    endif()
    if(MSVC AND NOT CMAKE_CXX_COMPILER_VERSION MATCHES
            "^${_WITNESS_EXPECTED_MSVC_FAMILY}(\\.|$)")
        message(FATAL_ERROR
            "The selected compiler ${CMAKE_CXX_COMPILER_VERSION} is outside the pinned "
            "MSVC ${_WITNESS_EXPECTED_MSVC_FAMILY} ABI family. Review and update "
            "build-environment.json plus the Witness triplet before rebuilding dependencies.")
    endif()

    string(SHA256 _WITNESS_DEPENDENCY_SIGNATURE
        "${_WITNESS_MANIFEST_SHA}|${_WITNESS_REGISTRY_SHA}|${_WITNESS_TRIPLET_SHA}|${_WITNESS_ENVIRONMENT_SHA}|${_WITNESS_VCPKG_VERSION}")
    set(_WITNESS_DEPENDENCY_STAMP
        "${CMAKE_BINARY_DIR}/witness-dependency-environment.txt")

    if(WITNESS_BOOTSTRAP_DEPENDENCIES)
        file(WRITE "${_WITNESS_DEPENDENCY_STAMP}"
            "signature=${_WITNESS_DEPENDENCY_SIGNATURE}\n"
            "triplet=${VCPKG_TARGET_TRIPLET}\n"
            "vcpkg=${_WITNESS_VCPKG_VERSION}\n"
            "manifest_sha256=${_WITNESS_MANIFEST_SHA}\n"
            "registry_sha256=${_WITNESS_REGISTRY_SHA}\n"
            "triplet_sha256=${_WITNESS_TRIPLET_SHA}\n"
            "environment_sha256=${_WITNESS_ENVIRONMENT_SHA}\n"
            "cmake=${CMAKE_VERSION}\n"
            "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
            "generator=${CMAKE_GENERATOR}\n"
            "generator_toolset=${CMAKE_GENERATOR_TOOLSET}\n")
        message(STATUS "Bootstrapped managed dependency environment ${_WITNESS_DEPENDENCY_SIGNATURE}")
    else()
        if(NOT EXISTS "${_WITNESS_DEPENDENCY_STAMP}")
            message(FATAL_ERROR
                "Managed dependencies have not been bootstrapped in this build directory. "
                "Run scripts/bootstrap-dependencies.ps1, then configure again.")
        endif()
        file(STRINGS "${_WITNESS_DEPENDENCY_STAMP}" _WITNESS_STAMP_SIGNATURE
            REGEX "^signature=")
        if(NOT _WITNESS_STAMP_SIGNATURE STREQUAL
                "signature=${_WITNESS_DEPENDENCY_SIGNATURE}")
            message(FATAL_ERROR
                "The pinned dependency environment changed. Ordinary configure will not "
                "modify installed packages. Review vcpkg.json, vcpkg-configuration.json, "
                "the Witness triplet, and vcpkg version, then explicitly run "
                "scripts/bootstrap-dependencies.ps1 if the change is intended.")
        endif()
        message(STATUS
            "Using immutable managed dependency environment ${_WITNESS_DEPENDENCY_SIGNATURE}")
    endif()
endif()

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
    if(WITNESS_PROFILE_BUILD)
        add_compile_options(/Bt+ /d1reportTime)
    endif()
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

# Build the Vue UI and always synchronize it beside the selected configuration's
# executable. WitnessServer's POST_BUILD step does not run when the native target
# is already up to date, whereas this custom target runs whenever WitnessServer is
# requested.
find_program(NPM_EXECUTABLE npm)
if(NPM_EXECUTABLE)
    add_custom_target(WebUI
        COMMAND ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/web-ui"
        COMMENT "Building Vue 3 web UI"
    )

    # Keep this in a separate target. With Visual Studio generators npm resolves
    # to npm.cmd, and commands following it in the same generated batch block are
    # not reliably executed.
    add_custom_target(SyncWebUI
		COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:WitnessServer>/Web.new"
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_SOURCE_DIR}/WitnessServer/Web"
			"$<TARGET_FILE_DIR:WitnessServer>/Web.new"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:WitnessServer>/Web"
		COMMAND ${CMAKE_COMMAND} -E rename
			"$<TARGET_FILE_DIR:WitnessServer>/Web.new"
			"$<TARGET_FILE_DIR:WitnessServer>/Web"
        COMMENT "Synchronizing Vue 3 web UI beside WitnessServer"
    )
    add_dependencies(SyncWebUI WebUI)
    add_dependencies(WitnessServer SyncWebUI)
else()
    # Package the checked-in/prebuilt bundle on systems without npm. Use the
    # same staged publication so a running server never observes a partial tree.
    add_custom_command(TARGET WitnessServer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:WitnessServer>/Web.new"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/WitnessServer/Web"
            "$<TARGET_FILE_DIR:WitnessServer>/Web.new"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:WitnessServer>/Web"
        COMMAND ${CMAKE_COMMAND} -E rename
            "$<TARGET_FILE_DIR:WitnessServer>/Web.new"
            "$<TARGET_FILE_DIR:WitnessServer>/Web"
        COMMENT "Synchronizing prebuilt web UI beside WitnessServer"
    )
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
