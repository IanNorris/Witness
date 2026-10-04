# Shared by real configuration and isolated no-compiler policy regressions.
function(witness_prepare_managed_dependencies)
    if(WITNESS_MANAGED_DEPENDENCIES)
        set(requested "${WITNESS_BOOTSTRAP_DEPENDENCIES}")
        # Consume permission before project()/the toolchain can fail. A retry
        # must never inherit a previous invocation's installation permission.
        set(WITNESS_BOOTSTRAP_DEPENDENCIES OFF CACHE BOOL
            "Allow vcpkg to install/update the managed dependency environment" FORCE)
        set(WITNESS_BOOTSTRAP_DEPENDENCIES "${requested}" PARENT_SCOPE)
        set(VCPKG_MANIFEST_INSTALL "${requested}" CACHE BOOL
            "Install vcpkg manifest dependencies" FORCE)
    endif()
endfunction()

function(witness_finish_managed_dependency_configuration)
    if(WITNESS_MANAGED_DEPENDENCIES)
        set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "Install vcpkg manifest dependencies" FORCE)
        set(VCPKG_MANIFEST_INSTALL OFF PARENT_SCOPE)
    endif()
endfunction()

function(witness_normalized_text_sha256 output path)
    file(READ "${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\r" "\n" content "${content}")
    string(SHA256 digest "${content}")
    set(${output} "${digest}" PARENT_SCOPE)
endfunction()

function(witness_read_vcpkg_pins environment checkout_output tool_output)
    string(JSON schema ERROR_VARIABLE schema_error GET "${environment}" schema)
    if(schema_error OR NOT schema EQUAL 2)
        message(FATAL_ERROR "build-environment.json requires schema 2 with independent vcpkg checkout/tool pins.")
    endif()
    foreach(field vcpkgCheckoutCommit vcpkgToolCommit)
        string(JSON pin ERROR_VARIABLE pin_error GET "${environment}" ${field})
        string(LENGTH "${pin}" pin_length)
        if(pin_error OR NOT pin_length EQUAL 40 OR NOT pin MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "Invalid ${field}: require a full lowercase commit hash.")
        endif()
        set(${field} "${pin}")
    endforeach()
    set(${checkout_output} "${vcpkgCheckoutCommit}" PARENT_SCOPE)
    set(${tool_output} "${vcpkgToolCommit}" PARENT_SCOPE)
endfunction()

function(witness_require_vcpkg_checkout root expected)
    string(LENGTH "${expected}" commit_length)
    if(NOT commit_length EQUAL 40 OR NOT expected MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Invalid pinned vcpkg commit")
    endif()
    find_program(WITNESS_GIT_EXECUTABLE git REQUIRED)
    execute_process(COMMAND "${WITNESS_GIT_EXECUTABLE}" -C "${root}" rev-parse --show-toplevel
        RESULT_VARIABLE root_result OUTPUT_VARIABLE checkout_root OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${WITNESS_GIT_EXECUTABLE}" -C "${root}" rev-parse HEAD
        RESULT_VARIABLE head_result OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(REAL_PATH "${root}" actual_root)
    file(REAL_PATH "${checkout_root}" reported_root)
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${actual_root}" actual_root)
        string(TOLOWER "${reported_root}" reported_root)
    endif()
    if(NOT root_result EQUAL 0 OR NOT head_result EQUAL 0 OR
            NOT actual_root STREQUAL reported_root OR NOT head STREQUAL expected)
        message(FATAL_ERROR
            "The vcpkg toolchain checkout does not match pinned commit ${expected}. "
            "The executable version alone does not pin its scripts. "
            "Run scripts/bootstrap-vcpkg.ps1 or select a clean matching checkout.")
    endif()
    execute_process(COMMAND "${WITNESS_GIT_EXECUTABLE}" -C "${root}" status --porcelain --untracked-files=no
        RESULT_VARIABLE status_result OUTPUT_VARIABLE tracked_changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT status_result EQUAL 0 OR NOT tracked_changes STREQUAL "")
        message(FATAL_ERROR "The pinned vcpkg checkout has tracked changes; review them before bootstrap/configure.")
    endif()
endfunction()

function(witness_validate_vcpkg_version version expected)
    string(FIND "${version}" "${expected}" commit_position)
    if(commit_position EQUAL -1)
        message(FATAL_ERROR "The vcpkg executable does not match pinned commit ${expected}.")
    endif()
endfunction()

function(witness_require_vcpkg_version root expected output)
    if(CMAKE_HOST_WIN32)
        set(executable "${root}/vcpkg.exe")
    else()
        set(executable "${root}/vcpkg")
    endif()
    if(NOT EXISTS "${executable}")
        message(FATAL_ERROR "Cannot fingerprint vcpkg at ${executable}")
    endif()
    execute_process(COMMAND "${executable}" version OUTPUT_VARIABLE version
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not query the selected vcpkg version.")
    endif()
    witness_validate_vcpkg_version("${version}" "${expected}")
    set(${output} "${version}" PARENT_SCOPE)
endfunction()

function(witness_require_msvc_family actual expected)
    if(NOT expected MATCHES "^[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Invalid pinned MSVC ABI family: ${expected}")
    endif()
    string(REPLACE "." "\\." escaped "${expected}")
    if(NOT actual MATCHES "^[0-9]+(\\.[0-9]+)*$" OR NOT actual MATCHES "^${escaped}(\\.|$)")
        message(FATAL_ERROR
            "The selected compiler ${actual} is outside the pinned MSVC ${expected} ABI family. "
            "Review build-environment.json plus the Witness triplet before rebuilding dependencies.")
    endif()
endfunction()

function(witness_require_dependency_stamp path expected triplet)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "Managed dependencies have not been bootstrapped in this build directory. "
            "Run scripts/bootstrap-dependencies.ps1, then configure again.")
    endif()
    file(STRINGS "${path}" signatures REGEX "^signature=")
    file(STRINGS "${path}" triplets REGEX "^triplet=")
    if(NOT signatures STREQUAL "signature=${expected}" OR NOT triplets STREQUAL "triplet=${triplet}")
        message(FATAL_ERROR
            "The pinned dependency environment changed. Ordinary configure will not modify installed packages. "
            "Review vcpkg.json, vcpkg-configuration.json, the Witness triplet, and vcpkg version, "
            "then explicitly run scripts/bootstrap-dependencies.ps1 if the change is intended.")
    endif()
endfunction()
