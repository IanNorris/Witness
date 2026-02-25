cmake_minimum_required(VERSION 3.20)
project(Witness LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(WitnessCamera)
add_subdirectory(WitnessServer)
