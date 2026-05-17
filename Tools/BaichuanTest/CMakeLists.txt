cmake_minimum_required(VERSION 3.20)
project(BaichuanTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(BaichuanTest main.cpp)
target_link_libraries(BaichuanTest PRIVATE OpenSSL::SSL OpenSSL::Crypto ws2_32)
