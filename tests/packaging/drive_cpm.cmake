# Drives the CPMAddPackage(GIT_REPOSITORY ...) story end to end, outside the
# build that is running this test: clone Sub0Log with CPM the same way a
# consumer's README-copied snippet would, then configure+build+run a consumer
# against nothing but that fetch. drive.cmake proves find_package() against an
# installed prefix; this proves the *other* documented path -- pasting a
# CPMAddPackage() block straight into a project -- because a README snippet
# nobody has run is a claim, not documentation.
#
# The GIT_REPOSITORY passed to CPM is this checkout itself (a plain local
# path) and GIT_TAG is its current HEAD commit, rather than the public GitHub
# URL. git's clone codepath does not care whether the remote is a URL or a
# local path -- only that it can resolve GIT_TAG in it -- so this exercises
# exactly the fetch machinery a real `GIT_REPOSITORY
# https://github.com/...` consumer goes through, while staying offline and
# independent of whatever commit happens to be pushed when this runs.
#
# Run via `cmake -P drive_cpm.cmake`, registered as a ctest by
# tests/packaging/CMakeLists.txt.

cmake_minimum_required(VERSION 3.21)

foreach(required_var SUB0LOG_SOURCE_DIR SUB0LOG_CONSUMER_DIR SUB0LOG_WORK_DIR)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "drive_cpm.cmake: ${required_var} was not passed with -D")
    endif()
endforeach()

# Start clean every run, same reasoning as drive.cmake: a stale consumer
# build left over from a previous failed run must not hide a broken fetch
# behind leftovers CMake's cache still remembers.
file(REMOVE_RECURSE "${SUB0LOG_WORK_DIR}")
file(MAKE_DIRECTORY "${SUB0LOG_WORK_DIR}")

set(consumer_build "${SUB0LOG_WORK_DIR}/consumer-build")

function(sub0log_run_or_die)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        string(REPLACE ";" " " pretty "${ARGV}")
        message(FATAL_ERROR "command failed (exit ${rc}): ${pretty}")
    endif()
endfunction()

# What CPM will fetch: HEAD of the checkout this test is itself running
# from. A clone only ever sees committed history, so this is exactly what a
# real GIT_REPOSITORY fetch would see too, dirty working tree or not.
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${SUB0LOG_SOURCE_DIR}"
    OUTPUT_VARIABLE sub0log_head_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE git_rc)
if(NOT git_rc EQUAL 0 OR sub0log_head_sha STREQUAL "")
    message(FATAL_ERROR
        "packaging::cpm_fetch: could not resolve HEAD in ${SUB0LOG_SOURCE_DIR}")
endif()

set(generator_args)
if(SUB0LOG_GENERATOR)
    list(APPEND generator_args -G "${SUB0LOG_GENERATOR}")
endif()

message(STATUS
    "packaging: CPMAddPackage fetching Sub0Log@${sub0log_head_sha} "
    "from ${SUB0LOG_SOURCE_DIR}")
sub0log_run_or_die(${CMAKE_COMMAND}
    -S "${SUB0LOG_CONSUMER_DIR}" -B "${consumer_build}"
    ${generator_args}
    "-DSUB0LOG_CPM_SCRIPT=${SUB0LOG_SOURCE_DIR}/cmake/CPM.cmake"
    "-DSUB0LOG_GIT_REPOSITORY=${SUB0LOG_SOURCE_DIR}"
    "-DSUB0LOG_GIT_TAG=${sub0log_head_sha}")

message(STATUS "packaging: building the CPM consumer")
sub0log_run_or_die(${CMAKE_COMMAND} --build "${consumer_build}" --config Debug)

if(WIN32)
    set(exe_name "sub0log_cpm_consumer.exe")
else()
    set(exe_name "sub0log_cpm_consumer")
endif()
# GLOB rather than a hard-coded path, same reasoning as drive.cmake:
# single-config generators put the binary directly under consumer_build,
# multi-config generators (Visual Studio) nest it under a per-config
# directory.
file(GLOB_RECURSE consumer_exe_candidates "${consumer_build}/*${exe_name}")
list(LENGTH consumer_exe_candidates n_found)
if(n_found EQUAL 0)
    message(FATAL_ERROR
        "packaging: no ${exe_name} found under ${consumer_build} after build")
endif()
list(GET consumer_exe_candidates 0 consumer_exe)

message(STATUS "packaging: running the CPM consumer")
sub0log_run_or_die("${consumer_exe}")

message(STATUS
    "packaging: OK -- CPMAddPackage(GIT_REPOSITORY ...) fetches and links Sub0Log")
