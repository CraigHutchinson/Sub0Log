cmake_minimum_required(VERSION 3.21)

# Detect if Sub0Log is being included inside another project.
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0Log VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0LOG_BUILD_TESTING  "Build unit-tests" ON)
option(SUB0LOG_BUILD_EXAMPLES "Build examples"   OFF)

# The project targets C++26 with a C++23 portability floor: every header must
# compile as C++23 (that is the interface contract below), and the project's
# own tests build in C++26 mode wherever the toolchain provides it. See
# docs/architecture.md, "Language standard".
set(SUB0LOG_CXX_STANDARD 26 CACHE STRING "C++ standard for Sub0Log's own tests/examples (floor 23)")

if(SUB0LOG_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
endif()

add_library(Sub0Log INTERFACE)
add_library(Sub0Log::Sub0Log ALIAS Sub0Log)

target_include_directories(Sub0Log INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The floor is a hard requirement, not a preference -- see REQUIREMENTS.md
# R8.2 and docs/architecture.md for how C++26 layers above it.
target_compile_features(Sub0Log INTERFACE cxx_std_23)

if(SUB0LOG_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # Resolve the standard the toolchain can actually deliver, once, here.
    if(SUB0LOG_CXX_STANDARD GREATER 23)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-std=c++${SUB0LOG_CXX_STANDARD}" SUB0LOG_HAS_STD_FLAG)
        if(NOT SUB0LOG_HAS_STD_FLAG AND NOT MSVC)
            message(STATUS "Sub0Log: compiler lacks -std=c++${SUB0LOG_CXX_STANDARD}; tests fall back to C++23")
            set(SUB0LOG_CXX_STANDARD 23)
        endif()
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
    add_subdirectory(tests)
endif()
