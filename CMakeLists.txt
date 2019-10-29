# Copyright (c) 2019 Harikrishnan (harikrishnan.prabakaran@gmail.com) Distributed under the MIT
# License. See accompanying file LICENSE.md or copy at http://opensource.org/licenses/MIT

cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PROJECT_NAME XactLogger)
set(EXEC ${PROJECT_NAME})

project(${PROJECT_NAME} C CXX)
set(PROJECT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "Building ${CMAKE_BUILD_TYPE} version!")

include(AdditionalBuildTypes.cmake)
include_directories(AFTER "include")

find_package(Threads)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(folly CONFIG REQUIRED)
set(SRC "src/main.cpp")

add_executable(${EXEC} ${SRC})
if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-stack-check")
endif(APPLE)
target_include_directories(${EXEC} PRIVATE ${Boost_INCLUDE_DIR})
target_link_libraries(${EXEC} PRIVATE ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
