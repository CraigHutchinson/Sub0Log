cmake_minimum_required(VERSION 3.21)

# Detect if Sub0Log is being included inside another project.
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0Log VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0LOG_BUILD_TESTING  "Build unit-tests" ON)
option(SUB0LOG_BUILD_EXAMPLES "Build examples"   OFF)

if(SUB0LOG_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
endif()

add_library(Sub0Log INTERFACE)
add_library(Sub0Log::Sub0Log ALIAS Sub0Log)

target_include_directories(Sub0Log INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# C++23 is a hard requirement, not a preference -- see REQUIREMENTS.md R8.2.
target_compile_features(Sub0Log INTERFACE cxx_std_23)

if(SUB0LOG_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
    add_subdirectory(tests)
endif()
