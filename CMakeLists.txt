cmake_minimum_required(VERSION 3.0)

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()

project(IPC)

add_library(IPC STATIC
    src/IPCBridge.cpp
)

target_include_directories(IPC PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>
    $<INSTALL_INTERFACE:include>)
