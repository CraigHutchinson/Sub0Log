# The Android lifecycle test app (issue #2): built with the NDK's toolchain
# file by build_apk.sh, never as part of the host build. Sub0Log comes in as
# an ordinary add_subdirectory() consumer.
cmake_minimum_required(VERSION 3.21)
project(Sub0LogAndroidLifecycle C CXX)

set(SUB0LOG_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SUB0LOG_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." sub0log EXCLUDE_FROM_ALL)

add_library(native_app_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(sub0log_lifecycle SHARED lifecycle.cpp)
set_target_properties(sub0log_lifecycle PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(sub0log_lifecycle PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sub0log_lifecycle PRIVATE Sub0Log::Sub0Log native_app_glue android log)
# The glue's entry point is only referenced by the framework, by name.
target_link_options(sub0log_lifecycle PRIVATE "-uANativeActivity_onCreate")
