# Tests only: the producer path keeps zero third-party dependencies (R8.3),
# so doctest is fetched here via CPM, never installed, never exported.
CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.4.12
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON"
)

# Test sources are globbed so an added test file is picked up on reconfigure.
file(GLOB SUB0LOG_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.test.cpp")

add_executable(Sub0LogTests ${SUB0LOG_TEST_SOURCES})
target_link_libraries(Sub0LogTests PRIVATE Sub0Log::Sub0Log doctest::doctest)

set_target_properties(Sub0LogTests PROPERTIES
    CXX_STANDARD ${SUB0LOG_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(NOT MSVC)
    target_compile_options(Sub0LogTests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(Sub0LogTests)
