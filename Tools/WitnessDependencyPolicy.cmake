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
