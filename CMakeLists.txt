cmake_minimum_required(VERSION 3.20)
project(csgc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CS:GO Legacy — 32-bit
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Please build for Win32! cmake -A Win32 ..")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# folder for the building result
set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist")

add_subdirectory(src)
add_subdirectory(launcher)