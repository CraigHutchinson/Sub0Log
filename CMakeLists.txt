cmake_minimum_required(VERSION 3.21)

# Detect if Sub0Log is being included inside another project.
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0Log VERSION 0.1.0 LANGUAGES CXX)

# include/sub0log/version.hpp hand-writes the same three numbers rather than
# being configure_file()-generated (its own header comment says why: a
# generated header breaks "just add include/ to your include path", the use
# a header-only library exists to preserve). Hand-written means two places
# say the version, so make configure fail rather than let them drift --
# cheaper than the alternative for anyone reading a mismatched
# SUB0LOG_VERSION_MAJOR after a release bump forgot one side.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/sub0log/version.hpp"
     _sub0log_version_major_line REGEX "^#define SUB0LOG_VERSION_MAJOR ")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/sub0log/version.hpp"
     _sub0log_version_minor_line REGEX "^#define SUB0LOG_VERSION_MINOR ")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/sub0log/version.hpp"
     _sub0log_version_patch_line REGEX "^#define SUB0LOG_VERSION_PATCH ")
string(REGEX REPLACE "^#define SUB0LOG_VERSION_MAJOR ([0-9]+)$" "\\1"
       _sub0log_header_major "${_sub0log_version_major_line}")
string(REGEX REPLACE "^#define SUB0LOG_VERSION_MINOR ([0-9]+)$" "\\1"
       _sub0log_header_minor "${_sub0log_version_minor_line}")
string(REGEX REPLACE "^#define SUB0LOG_VERSION_PATCH ([0-9]+)$" "\\1"
       _sub0log_header_patch "${_sub0log_version_patch_line}")
if(NOT _sub0log_header_major STREQUAL "${PROJECT_VERSION_MAJOR}"
   OR NOT _sub0log_header_minor STREQUAL "${PROJECT_VERSION_MINOR}"
   OR NOT _sub0log_header_patch STREQUAL "${PROJECT_VERSION_PATCH}")
    message(FATAL_ERROR
        "version.hpp (${_sub0log_header_major}.${_sub0log_header_minor}."
        "${_sub0log_header_patch}) disagrees with project(Sub0Log VERSION "
        "${PROJECT_VERSION}) -- update both together.")
endif()
# ...and re-run this check when the header changes, or a version bump that
# forgot project() would sail through on a stale configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/include/sub0log/version.hpp")

unset(_sub0log_version_major_line)
unset(_sub0log_version_minor_line)
unset(_sub0log_version_patch_line)
unset(_sub0log_header_major)
unset(_sub0log_header_minor)
unset(_sub0log_header_patch)

include(GNUInstallDirs)

option(SUB0LOG_BUILD_TESTING    "Build unit-tests" ON)
option(SUB0LOG_BUILD_TOOLS      "Build sub0log-cat" ON)
option(SUB0LOG_BUILD_EXAMPLES   "Build the runnable examples" OFF)
option(SUB0LOG_BUILD_BENCHMARKS "Build the KPI benchmark suite and the stress harness" OFF)

# Consumers embedding us via add_subdirectory() do not expect our install()
# rules to run when *they* install -- that is the standard convention (same
# shape as SUB0LOG_BUILD_TESTING already following IS_SUBPROJECT for
# enable_testing()), so default this off whenever we are not the top-level
# project rather than forcing every embedder to remember to turn it off.
if(IS_SUBPROJECT)
    option(SUB0LOG_INSTALL "Install Sub0Log and generate find_package() support" OFF)
else()
    option(SUB0LOG_INSTALL "Install Sub0Log and generate find_package() support" ON)
endif()

if(SUB0LOG_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
endif()

add_library(Sub0Log INTERFACE)
add_library(Sub0Log::Sub0Log ALIAS Sub0Log)

target_include_directories(Sub0Log INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# C++23 is a hard requirement, not a preference -- see REQUIREMENTS.md R8.2.
target_compile_features(Sub0Log INTERFACE cxx_std_23)

# The call-site macros use __VA_OPT__ (standard since C++20, and the reason a
# zero-argument site needs no separate spelling). MSVC implements it only under
# its conformant preprocessor, which /std:c++latest does NOT imply, so the
# legacy preprocessor mangles every call site into a syntax error. The
# requirement is real and belongs on the INTERFACE, where consumers inherit it
# rather than rediscovering it -- STYLE_GUIDE.md's rule is to use the standard
# feature and require standards conformance, not to work around its absence.
target_compile_options(Sub0Log INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# libatomic, only where the toolchain needs it. The producer path uses
# only atomics the target does lock-free (include/sub0log/detail/atomics.hpp
# picks 64-bit or the split 32-bit protocol accordingly), but a compiler may
# still lower an atomic operation to an out-of-line __atomic_* call --
# 32-bit Android ABIs and other 32-bit GCC/Clang targets are where this
# happens -- and those live in libatomic, which nothing links by default.
# Probed rather than assumed: compile and link the library's own atomic
# operations; if that fails, try again with libatomic and, if that works,
# hand it to every consumer on Sub0Log::Sub0Log's link interface. MSVC has
# no libatomic and needs none.
if(NOT MSVC)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    set(_sub0log_atomic_probe [=[
#include <sub0log/detail/atomics.hpp>
#include <sub0log/site.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
alignas(8) static std::byte gWords[16];
int main()
{
    sub0log::detail::storeHeadWord(gWords, 1u);
    const std::uint32_t claimed = sub0log::detail::claimChunkIndex(gWords + 8, 4u);
    sub0log::detail::RelaxedCounter counter;
    counter.increment();
    std::atomic<sub0log::detail::AnnounceWord> key{0};
    key.store(claimed + 1u, std::memory_order_relaxed);
    return static_cast<int>(sub0log::detail::loadHeadWord(gWords) + counter.load()
                            + key.load(std::memory_order_relaxed)
                            + sub0log::detail::loadClaimedChunks(gWords + 8, 4u));
}
]=])
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX23_STANDARD_COMPILE_OPTION}")
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles("${_sub0log_atomic_probe}" SUB0LOG_ATOMICS_LINK_WITHOUT_LIBATOMIC)
    if(NOT SUB0LOG_ATOMICS_LINK_WITHOUT_LIBATOMIC)
        set(CMAKE_REQUIRED_LIBRARIES atomic)
        check_cxx_source_compiles("${_sub0log_atomic_probe}" SUB0LOG_ATOMICS_LINK_WITH_LIBATOMIC)
        if(SUB0LOG_ATOMICS_LINK_WITH_LIBATOMIC)
            message(STATUS "Sub0Log: this toolchain lowers atomics to libatomic calls -- linking it")
            target_link_libraries(Sub0Log INTERFACE atomic)
        else()
            message(WARNING
                "Sub0Log: the atomic-operations probe does not link, with or without "
                "libatomic. Consumers will fail to link with undefined __atomic_* symbols; "
                "see CMakeFiles/CMakeConfigureLog.yaml for the probe's error.")
        endif()
    endif()
    cmake_pop_check_state()
    unset(_sub0log_atomic_probe)
endif()

if((SUB0LOG_BUILD_TESTING OR SUB0LOG_BUILD_BENCHMARKS)
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
endif()

# Before tests/, which registers a test that runs the built tool.
if(SUB0LOG_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

if(SUB0LOG_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

if(SUB0LOG_BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
    add_subdirectory(benchmarks)
endif()

# Examples are consumer code: they include <sub0log/...> and link the same
# target a consumer would. When testing is also on they are registered as
# ctest smoke tests (label `example`), because an example that nobody builds
# is an example that has already stopped compiling.
if(SUB0LOG_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()


# find_package(Sub0Log) support (docs/adoption-friction.md 1.1): a distro
# package, a vendored SDK, a superbuild, Conan or vcpkg all install into a
# prefix and then find_package() rather than add_subdirectory()/CPM -- until
# this block existed, none of them could consume Sub0Log at all.
if(SUB0LOG_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS Sub0Log EXPORT Sub0LogTargets)

    # The headers, not the target's build-tree include dir -- consumers get
    # $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> (set above) resolved
    # against wherever this actually lands.
    install(DIRECTORY include/sub0log
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")

    # Sub0Log::Sub0Log on the consumer side, matching the ALIAS above so
    # in-tree and installed consumers link the identical target name.
    # Everything the INTERFACE target carries -- cxx_std_23, the MSVC
    # /Zc:preprocessor option, the include dir -- rides along automatically:
    # they are usage requirements on the exported target itself, not
    # something this file re-derives. tests/packaging exercises this rather
    # than trusting it by inspection.
    install(EXPORT Sub0LogTargets
            NAMESPACE Sub0Log::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sub0Log)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sub0LogConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Sub0LogConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sub0Log)

    # SameMajorVersion, not ExactVersion or AnyNewerVersion. Two reasons,
    # both about which axis actually gates compatibility here:
    #  - Header-only means there is no .so/.dll for a consumer to be ABI
    #    compatible *with* -- every consumer recompiles from source, so an
    #    ABI-flavoured policy would be guarding against a failure mode this
    #    library cannot have.
    #  - The compatibility that does matter pre-1.0 is the wire format
    #    (wire::cFormatVersion, wire.hpp), which changes far slower than
    #    and independently of the library version this file gates. Tying
    #    find_package() version matching to major.minor.patch of a number
    #    that moves on every release, ahead of 1.0, would reject perfectly
    #    good upgrades; SameMajorVersion is the least presumptuous stock
    #    policy CMake ships until 1.0 gives PROJECT_VERSION_MAJOR itself a
    #    real compatibility meaning.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/Sub0LogConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/Sub0LogConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/Sub0LogConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sub0Log)
endif()
