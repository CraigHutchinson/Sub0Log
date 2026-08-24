cmake_minimum_required(VERSION 3.21)

# Detect if Sub0Log is being included inside another project.
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0Log VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0LOG_BUILD_TESTING    "Build unit-tests" ON)
option(SUB0LOG_BUILD_EXAMPLES   "Build examples"   OFF)
option(SUB0LOG_BUILD_BENCHMARKS "Build the KPI benchmark suite" OFF)

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

# The call-site macros use __VA_OPT__ (standard since C++20, and the reason a
# zero-argument site needs no separate spelling). MSVC implements it only under
# its conformant preprocessor, which /std:c++latest does NOT imply, so the
# legacy preprocessor mangles every call site into a syntax error. The
# requirement is real and belongs on the INTERFACE, where consumers inherit it
# rather than rediscovering it -- STYLE_GUIDE.md's rule is to use the standard
# feature and require standards conformance, not to work around its absence.
target_compile_options(Sub0Log INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

if((SUB0LOG_BUILD_TESTING OR SUB0LOG_BUILD_BENCHMARKS)
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
endif()

if(SUB0LOG_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

if(SUB0LOG_BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
    add_subdirectory(benchmarks)
endif()
