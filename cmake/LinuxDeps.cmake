# cmake/LinuxDeps.cmake
#
# Provides vcpkg-compatible IMPORTED targets for Linux/non-vcpkg builds.
# Included automatically from the root CMakeLists.txt when not on Windows
# and CMAKE_TOOLCHAIN_FILE does not reference vcpkg.
#
# Targets created:
#   Crow::Crow
#   unofficial-sodium::sodium
#   unofficial::sqlite3::sqlite3

cmake_minimum_required(VERSION 3.20)
find_package(PkgConfig REQUIRED)

# ---------------------------------------------------------------------------
# Crow (header-only)
# ---------------------------------------------------------------------------
# We intentionally skip find_package(Crow CONFIG) here because Crow's installed
# CrowConfig.cmake has a find_dependency(Boost) that fails on Ubuntu 24.04 with
# split Boost packages. Since Crow is header-only, we create the target manually.
if(NOT TARGET Crow::Crow)
    find_path(CROW_INCLUDE_DIR crow.h
        HINTS ENV CROW_ROOT ${CROW_ROOT}
        PATH_SUFFIXES include
    )
    if(NOT CROW_INCLUDE_DIR)
        message(FATAL_ERROR "Cannot find crow.h. Set CROW_ROOT or CROW_INCLUDE_DIR.")
    endif()

    # Crow needs standalone asio (header-only); system libasio-dev puts it in /usr/include
    find_path(ASIO_INCLUDE_DIR asio.hpp
        HINTS ENV ASIO_ROOT ${ASIO_ROOT}
        PATH_SUFFIXES include
    )
    if(NOT ASIO_INCLUDE_DIR)
        message(FATAL_ERROR "Cannot find asio.hpp. Install libasio-dev or set ASIO_ROOT.")
    endif()

    add_library(Crow::Crow INTERFACE IMPORTED)
    set_target_properties(Crow::Crow PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CROW_INCLUDE_DIR};${ASIO_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "CROW_STANDALONE_ASIO;CROW_ENABLE_SSL"
    )
    message(STATUS "Found Crow (manual): ${CROW_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------
# libsodium
# ---------------------------------------------------------------------------
if(NOT TARGET unofficial-sodium::sodium)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    add_library(unofficial-sodium::sodium INTERFACE IMPORTED)
    set_target_properties(unofficial-sodium::sodium PROPERTIES
        INTERFACE_LINK_LIBRARIES "PkgConfig::SODIUM"
    )
    message(STATUS "Found libsodium: ${SODIUM_VERSION}")
endif()

# ---------------------------------------------------------------------------
# SQLite3
# ---------------------------------------------------------------------------
if(NOT TARGET unofficial::sqlite3::sqlite3)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    add_library(unofficial_sqlite3 INTERFACE IMPORTED)
    add_library(unofficial::sqlite3::sqlite3 ALIAS unofficial_sqlite3)
    set_target_properties(unofficial_sqlite3 PROPERTIES
        INTERFACE_LINK_LIBRARIES "PkgConfig::SQLITE3"
    )
    message(STATUS "Found SQLite3: ${SQLITE3_VERSION}")
endif()

# ---------------------------------------------------------------------------
# FFmpeg (pkg-config based)
# ---------------------------------------------------------------------------
if(NOT FFMPEG_FOUND)
    pkg_check_modules(FFMPEG_AVCODEC    REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(FFMPEG_AVFORMAT   REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(FFMPEG_AVUTIL     REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(FFMPEG_SWSCALE    REQUIRED IMPORTED_TARGET libswscale)
    pkg_check_modules(FFMPEG_SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

    set(FFMPEG_FOUND TRUE)
    set(FFMPEG_INCLUDE_DIRS
        ${FFMPEG_AVCODEC_INCLUDE_DIRS}
        ${FFMPEG_AVFORMAT_INCLUDE_DIRS}
    )
    list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)

    set(FFMPEG_LIBRARIES
        PkgConfig::FFMPEG_AVCODEC
        PkgConfig::FFMPEG_AVFORMAT
        PkgConfig::FFMPEG_AVUTIL
        PkgConfig::FFMPEG_SWSCALE
        PkgConfig::FFMPEG_SWRESAMPLE
    )
    set(FFMPEG_LIBRARY_DIRS ${FFMPEG_AVCODEC_LIBRARY_DIRS})
    message(STATUS "Found FFmpeg: ${FFMPEG_AVCODEC_VERSION}")
endif()
