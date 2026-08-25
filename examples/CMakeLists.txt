# The runnable examples ladder (examples/README.md has the table). Each
# example is a single self-contained .cpp with its own main(), builds as an
# ordinary consumer of Sub0Log::Sub0Log (the same target any downstream
# project would link), and is registered as a ctest smoke test when
# SUB0LOG_BUILD_TESTING is also on -- an example nobody builds is an example
# that has already stopped compiling, per the root CMakeLists.txt comment
# that brought this file in.
#
# This file does not call enable_testing() or add_subdirectory(): both are
# the root project's job (root CMakeLists.txt already calls the former when
# SUB0LOG_BUILD_TESTING is on, and already add_subdirectory()s this
# directory when SUB0LOG_BUILD_EXAMPLES is on).

find_package(Threads REQUIRED)

# name -> whether it needs Threads::Threads (spawns std::thread) and whether
# it needs POSIX (fork/exec/signals) and is therefore skipped as a *build*
# target on Windows rather than merely a no-op at runtime -- 04/05/06 are
# fork()-based end to end and would not compile there, unlike 01/02/03/07/08
# which are portable and simply guard nothing.
set(SUB0LOG_EXAMPLES
    01_hello
    02_subsystems
    03_correlation
    04_crash_handler
    05_multi_process
    06_child_capture
    07_live_tail
    08_testing_your_code
    09_operating)

set(SUB0LOG_EXAMPLES_NEEDING_THREADS
    03_correlation
    07_live_tail)

# 04/05/06 build and run on POSIX; on Windows they are not compiled at all
# (fork(), execvp() and <sys/wait.h> do not exist there), matching how the
# library's own POSIX-only system tests are gated. Each still has a Windows
# arm in its ctest label story: nothing in the ladder disappears silently
# from `ctest -L example`'s point of view on a POSIX box, and on Windows the
# ladder is 01/02/03/07/08 until the v2 Windows arms land (docs/
# architecture.md's phasing).
set(SUB0LOG_EXAMPLES_POSIX_ONLY
    04_crash_handler
    05_multi_process
    06_child_capture)

foreach(example IN LISTS SUB0LOG_EXAMPLES)
    if(WIN32 AND example IN_LIST SUB0LOG_EXAMPLES_POSIX_ONLY)
        continue()
    endif()

    set(target "sub0log_example_${example}")
    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${example}.cpp")
    target_link_libraries(${target} PRIVATE Sub0Log::Sub0Log)

    if(example IN_LIST SUB0LOG_EXAMPLES_NEEDING_THREADS)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(SUB0LOG_BUILD_TESTING)
        add_test(NAME "example::${example}" COMMAND ${target})
        set_tests_properties("example::${example}" PROPERTIES LABELS "example")
    endif()
endforeach()
