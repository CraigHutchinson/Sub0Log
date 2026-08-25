# Drives the whole find_package(Sub0Log) story end to end, outside the
# build that is running this test: configure+install the library into a
# throwaway prefix, then configure+build+run a consumer against nothing but
# that prefix. Anything short of this -- reading CMakeLists.txt, trusting
# that install(EXPORT) is spelled right -- is inspection, not proof, and
# docs/adoption-friction.md 1.1 is exactly the finding "nobody tried it".
#
# Run via `cmake -P drive.cmake`, registered as a ctest by
# tests/packaging/CMakeLists.txt. Every step below either succeeds or the
# script stops with a FATAL_ERROR, so a broken find_package() shows up as a
# failed test, not a quiet log line.

cmake_minimum_required(VERSION 3.21)

foreach(required_var SUB0LOG_SOURCE_DIR SUB0LOG_CONSUMER_DIR SUB0LOG_WORK_DIR)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "drive.cmake: ${required_var} was not passed with -D")
    endif()
endforeach()

# Start clean every run -- a stale prefix left over from a previous failed
# run must not hide a broken install() behind leftovers find_package() can
# still see.
file(REMOVE_RECURSE "${SUB0LOG_WORK_DIR}")
file(MAKE_DIRECTORY "${SUB0LOG_WORK_DIR}")

set(prefix          "${SUB0LOG_WORK_DIR}/prefix")
set(producer_build   "${SUB0LOG_WORK_DIR}/producer-build")
set(consumer_build   "${SUB0LOG_WORK_DIR}/consumer-build")

function(sub0log_run_or_die)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        string(REPLACE ";" " " pretty "${ARGV}")
        message(FATAL_ERROR "command failed (exit ${rc}): ${pretty}")
    endif()
endfunction()

set(generator_args)
if(SUB0LOG_GENERATOR)
    list(APPEND generator_args -G "${SUB0LOG_GENERATOR}")
endif()

message(STATUS "packaging: configuring Sub0Log itself for install into ${prefix}")
sub0log_run_or_die(${CMAKE_COMMAND}
    -S "${SUB0LOG_SOURCE_DIR}" -B "${producer_build}"
    ${generator_args}
    "-DCMAKE_INSTALL_PREFIX=${prefix}"
    # Testing/examples/benchmarks pull in CPM and doctest (SUB0LOG_BUILD_TESTING
    # normally defaults ON); none of that belongs in what gets installed, and
    # skipping it keeps this test's own wall-clock cost down.
    -DSUB0LOG_BUILD_TESTING=OFF
    -DSUB0LOG_BUILD_EXAMPLES=OFF
    -DSUB0LOG_BUILD_BENCHMARKS=OFF
    -DSUB0LOG_INSTALL=ON)

# Built before installing, not because a header-only library needs
# compiling but because sub0log-cat is an install target now: a package
# that ships headers and no reader leaves a consumer where they started,
# so the tool being installable is part of what this test checks.
message(STATUS "packaging: building (sub0log-cat is an install target)")
sub0log_run_or_die(${CMAKE_COMMAND} --build "${producer_build}" --config Debug)

message(STATUS "packaging: installing Sub0Log")
sub0log_run_or_die(${CMAKE_COMMAND} --install "${producer_build}" --config Debug)

message(STATUS "packaging: configuring the consumer against the installed prefix only")
sub0log_run_or_die(${CMAKE_COMMAND}
    -S "${SUB0LOG_CONSUMER_DIR}" -B "${consumer_build}"
    ${generator_args}
    "-DCMAKE_PREFIX_PATH=${prefix}")

message(STATUS "packaging: building the consumer")
sub0log_run_or_die(${CMAKE_COMMAND} --build "${consumer_build}" --config Debug)

if(WIN32)
    set(exe_name "sub0log_packaging_consumer.exe")
else()
    set(exe_name "sub0log_packaging_consumer")
endif()
# GLOB rather than a hard-coded path: single-config generators (Ninja,
# Makefiles) put the binary directly under consumer_build, multi-config
# generators (Visual Studio) nest it under a per-config directory.
file(GLOB_RECURSE consumer_exe_candidates "${consumer_build}/*${exe_name}")
list(LENGTH consumer_exe_candidates n_found)
if(n_found EQUAL 0)
    message(FATAL_ERROR "packaging: no ${exe_name} found under ${consumer_build} after build")
endif()
list(GET consumer_exe_candidates 0 consumer_exe)

message(STATUS "packaging: running the consumer")
sub0log_run_or_die("${consumer_exe}")

message(STATUS "packaging: OK -- find_package(Sub0Log) works from an installed prefix")
