cmake_minimum_required(VERSION 3.20)
project(REChiaro VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8)
    # Statically link MSVC runtime library (/MT) for release builds
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(${PROJECT_NAME} SHARED 
    src/main.cpp
    src/reshade_bridge.cpp
    src/mcp_server.cpp
    src/reshade_bridge.hpp
    src/mcp_server.hpp
    src/version.h
    deps/base64.hpp
)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)

# Determine addon suffix based on target architecture
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ADDON_SUFFIX ".addon64")
else()
    set(ADDON_SUFFIX ".addon32")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES 
    PREFIX ""
    SUFFIX ${ADDON_SUFFIX}
)

# Link against Windows socket and system libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ws2_32
    user32
    kernel32
    version
)
