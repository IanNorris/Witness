# Witness dependency ABI epoch: 1
#
# Bump the epoch (and explain why in the commit) when a genuine dependency ABI
# rebuild is required. The triplet file participates in vcpkg's ABI hash.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# VS 2026 decouples its compiler package from the IDE. Stay within the 14.51
# ABI family while allowing servicing releases such as 14.51.xxxxx.
set(VCPKG_PLATFORM_TOOLSET v145)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.51)

# Witness development and deployment use release dependencies. This prevents
# every large port being compiled a second time merely for RelWithDebInfo work.
set(VCPKG_BUILD_TYPE release)

# Servicing updates inside the explicitly selected ABI family must not turn the
# compiler executable hash into a rebuild of the world. The family and epoch
# above remain tracked because this triplet itself is part of every port ABI.
set(VCPKG_DISABLE_COMPILER_TRACKING ON)
