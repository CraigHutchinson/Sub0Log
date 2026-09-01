# The runnable examples ladder (examples/README.md has the table). Each
# example is a single self-contained .cpp with its own main(), builds as an
# ordinary consumer of Sub0Log::Sub0Log (the same target any downstream
# project would link), and is registered as a ctest smoke test when
# SUB0LOG_BUILD_TESTING is also on -- an example nobody builds is an example
# that has already stopped compiling, per the root CMakeLists.txt comment
# that brought this file in. 12_plugin_abi is the one exception to "single
# .cpp" (a plugin, by definition, cannot be the same translation unit as its
# host) and is wired up separately, below the ladder proper.
#
# This file does not call enable_testing() or add_subdirectory(): both are
# the root project's job (root CMakeLists.txt already calls the former when
# SUB0LOG_BUILD_TESTING is on, and already add_subdirectory()s this
# directory when SUB0LOG_BUILD_EXAMPLES is on).

find_package(Threads REQUIRED)

# name -> whether it needs Threads::Threads (spawns std::thread) and whether
# it needs POSIX (fork() specifically -- not "spawns a child process": since
# v2, ChildProcess::spawn() has a real Windows arm (CreateProcessW), so
# 06/10, which only spawn through it, build and run everywhere. 04 and 05
# call raw fork() directly (04 to put a signal handler in a child that is
# then killed; 05 to run the *same* binary twice, sharing address space
# until it diverges) -- fork() itself has no Windows equivalent and none is
# on this project's roadmap (docs/architecture.md's phasing), so those two
# stay POSIX-only build targets, not merely no-ops at runtime.
set(SUB0LOG_EXAMPLES
    01_hello
    02_subsystems
    03_correlation
    04_crash_handler
    05_multi_process
    06_child_capture
    07_live_tail
    08_testing_your_code
    09_operating
    10_child_capture_git
    11_continuation_chains)

set(SUB0LOG_EXAMPLES_NEEDING_THREADS
    03_correlation
    07_live_tail)

# Only fork()-based examples are excluded as *build* targets on Windows
# (fork() does not exist there, so these do not compile, let alone run --
# see the comment above SUB0LOG_EXAMPLES for why 06/10 are not on this
# list despite spawning a child process). Each still has a Windows arm in
# its ctest label story: nothing in the ladder disappears silently from
# `ctest -L example`'s point of view on a POSIX box, and on Windows the
# ladder is every example except these two.
set(SUB0LOG_EXAMPLES_POSIX_ONLY
    04_crash_handler
    05_multi_process)

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

# ---------------------------------------------------------------------------
# 0_workedexample: the flagship. Deliberately outside SUB0LOG_EXAMPLES (it is
# not a rung on the ladder, and it is the one example in this directory that
# needs a third-party dependency, FTXUI, for its terminal UI) -- wired up
# here the same way 12_plugin_abi is its own block below rather than forced
# into the loop above.
#
# CPM.cmake is normally included once by the root CMakeLists.txt, but only
# when SUB0LOG_BUILD_TESTING or SUB0LOG_BUILD_BENCHMARKS is also on (see its
# own comment); a build with only SUB0LOG_BUILD_EXAMPLES=ON would otherwise
# reach this file with CPMAddPackage undefined. include()-ing CPM.cmake a
# second time is harmless (its own top-of-file guard against that is the
# first thing it does), so this fetches FTXUI only when this one target is
# actually being configured, without assuming what else the caller turned on.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CPM.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CPM.cmake")

    CPMAddPackage(
        NAME ftxui
        GITHUB_REPOSITORY ArthurSonzogni/FTXUI
        GIT_TAG v5.0.0
        OPTIONS "FTXUI_BUILD_EXAMPLES OFF" "FTXUI_BUILD_TESTS OFF" "FTXUI_ENABLE_INSTALL OFF")

    add_executable(sub0log_example_0_workedexample "${CMAKE_CURRENT_SOURCE_DIR}/0_workedexample.cpp")
    target_link_libraries(sub0log_example_0_workedexample PRIVATE
        Sub0Log::Sub0Log
        Threads::Threads
        ftxui::screen
        ftxui::dom
        ftxui::component)
    set_target_properties(sub0log_example_0_workedexample PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(sub0log_example_0_workedexample PRIVATE /W4)
    else()
        target_compile_options(sub0log_example_0_workedexample PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(SUB0LOG_BUILD_TESTING)
        # No --smoke needed to make this ctest-safe: ctest runs every test
        # with no controlling terminal by default, and 0_workedexample.cpp's
        # own isatty()/_isatty() check already routes that straight to its
        # headless path (the file's own comment explains why both checks
        # exist regardless). Passed anyway, so the test stays correct even
        # if ctest's own tty behaviour ever changed underneath it.
        add_test(NAME "example::0_workedexample"
                 COMMAND sub0log_example_0_workedexample --smoke)
        set_tests_properties("example::0_workedexample" PROPERTIES LABELS "example")
    endif()
else()
    message(WARNING
        "examples/CMakeLists.txt: cmake/CPM.cmake not found -- skipping "
        "sub0log_example_0_workedexample (needs FTXUI via CPM).")
endif()

# ---------------------------------------------------------------------------
# 12_plugin_abi: a host and a plugin, because that is the shape R4 actually
# has -- a plugin that could include <sub0log/log.hpp> would not be
# demonstrating anything (docs/adoption-friction.md 2.3 is what a plugin
# doing that actually gets: every one of its own records, silently). Modeled
# on tests/system/abi.test.cpp + tests/system/plugin/abi_test_plugin.cpp,
# which prove the same thing for the test suite; this is the teaching
# version, built and run the same way SUB0LOG_BUILD_EXAMPLES already builds
# and runs the rest of the ladder.

add_library(sub0log_example_12_plugin_abi_plugin MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin_abi/plugin.cpp")
# Deliberately not Sub0Log::Sub0Log: the raw include path only, so this
# target carries no dependency on the library it is proving a plugin does
# not need (R4.1/R4.2) -- see plugin.cpp's own header comment.
target_include_directories(sub0log_example_12_plugin_abi_plugin PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(sub0log_example_12_plugin_abi_plugin PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "sub0log_example_12_plugin")
if(MSVC)
    target_compile_options(sub0log_example_12_plugin_abi_plugin PRIVATE /W4)
else()
    target_compile_options(sub0log_example_12_plugin_abi_plugin PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(sub0log_example_12_plugin_abi_host "${CMAKE_CURRENT_SOURCE_DIR}/plugin_abi/host.cpp")
target_link_libraries(sub0log_example_12_plugin_abi_host PRIVATE Sub0Log::Sub0Log)
target_compile_definitions(sub0log_example_12_plugin_abi_host PRIVATE
    "SUB0LOG_EXAMPLE_PLUGIN_PATH=\"$<TARGET_FILE:sub0log_example_12_plugin_abi_plugin>\"")
set_target_properties(sub0log_example_12_plugin_abi_host PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "sub0log_example_12_plugin_abi_host"
    # The host exports sub0log_abi_v1 (SUB0LOG_ABI_HOST_EXPORT()) and this
    # resolves it back through the dynamic loader itself, the same way a
    # real plugin would, rather than trusting that the direct C++ call also
    # proves the export worked -- ENABLE_EXPORTS is what makes the symbol
    # visible to dlsym/GetProcAddress at all (tests/CMakeLists.txt's
    # identical comment on Sub0LogSystemTests explains the mechanism).
    ENABLE_EXPORTS ON)
if(MSVC)
    target_compile_options(sub0log_example_12_plugin_abi_host PRIVATE /W4)
else()
    target_compile_options(sub0log_example_12_plugin_abi_host PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NOT WIN32)
    target_link_libraries(sub0log_example_12_plugin_abi_host PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(sub0log_example_12_plugin_abi_host sub0log_example_12_plugin_abi_plugin)

if(SUB0LOG_BUILD_TESTING)
    add_test(NAME "example::12_plugin_abi" COMMAND sub0log_example_12_plugin_abi_host)
    set_tests_properties("example::12_plugin_abi" PROPERTIES LABELS "example")
endif()
