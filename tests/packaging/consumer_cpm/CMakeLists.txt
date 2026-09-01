cmake_minimum_required(VERSION 3.21)
project(Sub0LogCpmConsumer LANGUAGES CXX)

# SUB0LOG_CPM_SCRIPT, SUB0LOG_GIT_REPOSITORY and SUB0LOG_GIT_TAG are passed by
# drive_cpm.cmake. A real consumer fetches CPM.cmake with the one-line
# download snippet from https://github.com/cpm-cmake/CPM.cmake -- that
# download is CPM's own concern, already covered by its own test suite, and
# is not what this project exists to prove. What this project *does* prove:
# that CPMAddPackage(GIT_REPOSITORY ... GIT_TAG ...) actually clones Sub0Log
# and makes Sub0Log::Sub0Log linkable, which is the exact block
# README.md tells a new consumer to paste.
if(NOT DEFINED SUB0LOG_CPM_SCRIPT)
    message(FATAL_ERROR "SUB0LOG_CPM_SCRIPT was not passed with -D")
endif()
include("${SUB0LOG_CPM_SCRIPT}")

CPMAddPackage(
    NAME Sub0Log
    GIT_REPOSITORY "${SUB0LOG_GIT_REPOSITORY}"
    GIT_TAG "${SUB0LOG_GIT_TAG}"
    OPTIONS
        "SUB0LOG_BUILD_TESTING OFF"
        "SUB0LOG_BUILD_TOOLS OFF"
        "SUB0LOG_BUILD_EXAMPLES OFF"
        "SUB0LOG_BUILD_BENCHMARKS OFF"
        "SUB0LOG_INSTALL OFF")

add_executable(sub0log_cpm_consumer main.cpp)
target_link_libraries(sub0log_cpm_consumer PRIVATE Sub0Log::Sub0Log)

# As in tests/packaging/consumer, deliberately no CXX_STANDARD or MSVC
# preprocessor flag here: both must arrive purely as usage requirements
# carried by Sub0Log::Sub0Log, whether the target came from find_package()
# or, as here, from add_subdirectory() underneath CPMAddPackage().
