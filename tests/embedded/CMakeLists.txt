# The embedded producer path (issue #2): Logger::createInMemory held to a
# firmware/NDK consumer's constraints -- no exceptions, no RTTI, no heap,
# no file -- by building and running it that way, then decoding what it
# wrote with the desktop tool. See producer.cpp for what each check is.

add_executable(Sub0LogEmbeddedProducer "${CMAKE_CURRENT_SOURCE_DIR}/producer.cpp")
target_link_libraries(Sub0LogEmbeddedProducer PRIVATE Sub0Log::Sub0Log)
set_target_properties(Sub0LogEmbeddedProducer PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
if(MSVC)
    # Only RTTI is turned off here: MSVC's STL under _HAS_EXCEPTIONS=0 is not
    # a configuration Microsoft supports, so the no-exceptions half of the
    # claim is carried by the GCC/Clang legs.
    target_compile_options(Sub0LogEmbeddedProducer PRIVATE /W4 /GR-)
    # fopen in the dump step: parsed and closed immediately, nothing retained.
    target_compile_definitions(Sub0LogEmbeddedProducer PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(Sub0LogEmbeddedProducer PRIVATE
        -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()

set(dump "${CMAKE_CURRENT_BINARY_DIR}/embedded-producer.s0l")
add_test(NAME "embedded::producer" COMMAND Sub0LogEmbeddedProducer "${dump}")
set_tests_properties("embedded::producer" PROPERTIES
    LABELS "integration;embedded"
    FIXTURES_SETUP sub0log_embedded_dump)

if(TARGET sub0log-cat)
    add_test(NAME "embedded::decode" COMMAND sub0log-cat --stats "${dump}")
    set_tests_properties("embedded::decode" PROPERTIES
        LABELS "integration;embedded"
        FIXTURES_REQUIRED sub0log_embedded_dump
        PASS_REGULAR_EXPRESSION "embedded sample 3 of imu")
endif()

# The OS-less platform arm, built and run on the host so it cannot rot
# between runs of CI's cross-compiling embedded-arm job. Not on Windows:
# the probe's point is "no OS header", and a Windows build is always an OS.
if(NOT WIN32)
    add_executable(Sub0LogFreestandingProbe "${CMAKE_CURRENT_SOURCE_DIR}/freestanding_probe.cpp")
    target_link_libraries(Sub0LogFreestandingProbe PRIVATE Sub0Log::Sub0Log)
    target_compile_definitions(Sub0LogFreestandingProbe PRIVATE SUB0LOG_PLATFORM_CUSTOM)
    set_target_properties(Sub0LogFreestandingProbe PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(Sub0LogFreestandingProbe PRIVATE
        -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    add_test(NAME "embedded::freestanding_probe" COMMAND Sub0LogFreestandingProbe)
    set_tests_properties("embedded::freestanding_probe" PROPERTIES
        LABELS "integration;embedded")
endif()
