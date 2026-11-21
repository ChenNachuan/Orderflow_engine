cmake_minimum_required(VERSION 3.20)

project(Orderflow_engine
    VERSION 0.1.0
    DESCRIPTION "High-performance C++ orderflow processing and market microstructure engine"
    LANGUAGES CXX
)

# ------------------------------------------------------------------------------
# Global C++ Settings
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
option(ORDERFLOW_ENGINE_BUILD_TESTS     "Build unit tests" OFF)
option(ORDERFLOW_ENGINE_BUILD_EXAMPLES  "Build example programs" OFF)
option(ORDERFLOW_ENGINE_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

# ------------------------------------------------------------------------------
# Sources & Headers
# ------------------------------------------------------------------------------
file(GLOB_RECURSE ORDERFLOW_ENGINE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

file(GLOB_RECURSE ORDERFLOW_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
)

# ------------------------------------------------------------------------------
# Library Target: orderflow_engine
# ------------------------------------------------------------------------------
if(ORDERFLOW_ENGINE_SOURCES)
    add_library(orderflow_engine STATIC
        ${ORDERFLOW_ENGINE_SOURCES}
        ${ORDERFLOW_ENGINE_HEADERS}
    )
    set(ORDERFLOW_ENGINE_VISIBILITY PUBLIC)

    if(ORDERFLOW_ENGINE_ENABLE_WARNINGS)
        if(MSVC)
            target_compile_options(orderflow_engine PRIVATE /W4 /permissive-)
        else()
            target_compile_options(orderflow_engine PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
else()
    # Header-only mode while no source files exist under src/
    add_library(orderflow_engine INTERFACE
        ${ORDERFLOW_ENGINE_HEADERS}
    )
    set(ORDERFLOW_ENGINE_VISIBILITY INTERFACE)
endif()

add_library(orderflow_engine::orderflow_engine ALIAS orderflow_engine)

target_include_directories(orderflow_engine ${ORDERFLOW_ENGINE_VISIBILITY}
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(orderflow_engine ${ORDERFLOW_ENGINE_VISIBILITY} cxx_std_20)

# ------------------------------------------------------------------------------
# Subdirectories (Tests / Examples)
# ------------------------------------------------------------------------------
if(ORDERFLOW_ENGINE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

if(ORDERFLOW_ENGINE_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
