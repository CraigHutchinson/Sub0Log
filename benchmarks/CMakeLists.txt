# The KPI benchmark suite (see benchmarks/README.md). CPM.cmake is already
# included by the root CMakeLists when SUB0LOG_BUILD_BENCHMARKS is ON
# (mirrors the pattern tests/CMakeLists.txt uses for doctest), so this file
# only fetches nanobench and declares the one benchmark executable.
CPMAddPackage(
    NAME nanobench
    GITHUB_REPOSITORY martinus/nanobench
    GIT_TAG v4.3.11
)

# Benchmarks are meaningless below Release: an unoptimised build's timings
# do not reflect the producer-path claims (R1, R1.4) this suite exists to
# measure. Multi-config generators (Visual Studio, Xcode) pick the
# configuration at build time, so there is nothing to check here; warn only
# for the single-config case, where CMAKE_BUILD_TYPE is decided now.
get_property(sub0logIsMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT sub0logIsMultiConfig AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    message(WARNING
        "SUB0LOG_BUILD_BENCHMARKS is ON but CMAKE_BUILD_TYPE is "
        "'${CMAKE_BUILD_TYPE}', not Release or RelWithDebInfo -- benchmark "
        "numbers from this build will not be meaningful. See "
        "benchmarks/README.md.")
endif()

# std::thread (the "throughput" KPI group spawns producer threads).
find_package(Threads REQUIRED)

add_executable(Sub0LogBenchmarks
    main.cpp
    emit.bench.cpp
    claim.bench.cpp
    throughput.bench.cpp
    decode.bench.cpp
    merge.bench.cpp
    format.bench.cpp
)
target_link_libraries(Sub0LogBenchmarks PRIVATE
    Sub0Log::Sub0Log
    nanobench
    Threads::Threads
)
target_include_directories(Sub0LogBenchmarks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(Sub0LogBenchmarks PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(Sub0LogBenchmarks PRIVATE /W4)
else()
    target_compile_options(Sub0LogBenchmarks PRIVATE -Wall -Wextra -Wpedantic)
endif()
