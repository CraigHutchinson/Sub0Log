# Tests only: the producer path keeps zero third-party dependencies (R8.3),
# so doctest is fetched here via CPM, never installed, never exported.
CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.4.12
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON"
)
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

# The R4 round trip's plugin (tests/system/abi.test.cpp): a MODULE library,
# not SHARED, because this is built to be dlopen()'d and never linked
# against. It includes only sub0log_abi.h -- target_include_directories
# below is deliberately the raw include path rather than
# Sub0Log::Sub0Log, so this target carries no dependency on the library it
# is proving a plugin does not need (R4.1/R4.2).
#
# -fvisibility=hidden on POSIX is not a hardening default here, it is the
# point: docs/adoption-friction.md 2.3 measured this exact configuration
# losing every one of a plugin's own records under the header-only path, and
# CXX_VISIBILITY_PRESET is what reproduces it. MSVC needs nothing extra --
# nothing is exported from a DLL there unless __declspec(dllexport) asks,
# which is already the only behaviour available (sub0log_abi.h's own header
# comment).
add_library(Sub0LogAbiTestPlugin MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/system/plugin/abi_test_plugin.cpp")
target_include_directories(Sub0LogAbiTestPlugin PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(Sub0LogAbiTestPlugin PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
    target_compile_options(Sub0LogAbiTestPlugin PRIVATE /W4)
else()
    target_compile_options(Sub0LogAbiTestPlugin PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

    # The system suite runs the shipped tool as a subprocess when it was
    # built; without it those cases compile away rather than silently
    # passing, so `ctest` on a -DSUB0LOG_BUILD_TOOLS=OFF build does not
    # claim coverage it does not have.
    if(TARGET sub0log-cat)
        target_compile_definitions(${target} PRIVATE
            "SUB0LOG_CAT_PATH=\"$<TARGET_FILE:sub0log-cat>\"")
    endif()

    doctest_discover_tests(${target}
        TEST_PREFIX "${label}::"
        PROPERTIES LABELS "${label}")
endfunction()

sub0log_add_test_category(Unit)
sub0log_add_test_category(Integration)
sub0log_add_test_category(System)

# R4's dlopen round trip (tests/system/abi.test.cpp) needs two things the
# other system tests do not:
#  - the built plugin's path, the same way SUB0LOG_CAT_PATH already hands
#    the system suite the built tool's path above;
#  - its own defined symbols pushed into a place `dlsym`/`GetProcAddress`
#    can find them. Sub0LogSystemTests exports sub0log_abi_v1
#    (SUB0LOG_ABI_HOST_EXPORT in abi.test.cpp) and then resolves it back
#    through the dynamic loader to prove the export macro really exported
#    it (R4.1) rather than trusting the direct C++ call would also have
#    proved that. On POSIX that requires linking with -rdynamic; on
#    Windows it is what makes GetProcAddress(GetModuleHandle(NULL), ...)
#    able to see a symbol in the .exe at all. ENABLE_EXPORTS is CMake's one
#    spelling for both.
if(TARGET Sub0LogAbiTestPlugin)
    target_compile_definitions(Sub0LogSystemTests PRIVATE
        "SUB0LOG_ABI_PLUGIN_PATH=\"$<TARGET_FILE:Sub0LogAbiTestPlugin>\"")
    set_target_properties(Sub0LogSystemTests PROPERTIES ENABLE_EXPORTS ON)
    if(NOT WIN32)
        target_link_libraries(Sub0LogSystemTests PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

# Not a fourth doctest category (sub0log_add_test_category): it doesn't
# exercise wire.hpp/encode.hpp against the in-tree target at all, it proves
# an out-of-tree find_package(Sub0Log) works against an installed one.
# Different question, different mechanism -- see tests/packaging/CMakeLists.txt.
add_subdirectory(packaging)
