# Tests only: the producer path keeps zero third-party dependencies (R8.3),
# so doctest is fetched here via CPM, never installed, never exported.
CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.4.12
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON"
)
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

# Three categories, three executables, three ctest labels:
#   unit         pure logic against wire.hpp/encode.hpp, no I/O, no processes
#   integration  components against real files or hand-built images, one process
#   system       whole-library behavior: hard kills, multiple processes,
#                spawned children -- everything R3/R5 promise end to end
#
# Run one category with `ctest -L unit` (or integration/system); the split is
# also what lets CI run the cheap categories on every configuration and the
# process-spawning ones only where the platform arm exists.
function(sub0log_add_test_category CATEGORY)
    set(target "Sub0Log${CATEGORY}Tests")
    string(TOLOWER "${CATEGORY}" label)
    file(GLOB sources CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/${label}/*.test.cpp")
    if(NOT sources)
        message(FATAL_ERROR "test category '${label}' matched no sources")
    endif()

    add_executable(${target} ${sources}
                   "${CMAKE_CURRENT_SOURCE_DIR}/support/doctest_main.cpp")
    target_link_libraries(${target} PRIVATE Sub0Log::Sub0Log doctest::doctest)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    doctest_discover_tests(${target}
        TEST_PREFIX "${label}::"
        PROPERTIES LABELS "${label}")
endfunction()

sub0log_add_test_category(Unit)
sub0log_add_test_category(Integration)
sub0log_add_test_category(System)
