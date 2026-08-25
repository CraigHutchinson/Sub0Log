// The whole point of this file: it must compile and link against nothing
// but an installed Sub0Log -- no add_subdirectory, no CPM, no reference to
// this repository's include/ or cmake/ trees, only find_package(). If
// install(EXPORT) or the generated Sub0LogConfig.cmake ever regress, this
// consumer project (driven by tests/packaging/drive.cmake) is where that
// shows up as a hard failure rather than a documentation gap
// (docs/adoption-friction.md 1.1).

#include <sub0log/log.hpp>
#include <sub0log/version.hpp>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <type_traits>
#include <utility> // std::to_underlying -- see the static_assert below

namespace {
constexpr sub0log::SubsystemId cConsumer{1};
}

// This project's CMakeLists.txt sets no C++ standard of its own -- C++23
// must arrive as a usage requirement from Sub0Log::Sub0Log (the
// target_compile_features(... cxx_std_23) in the root CMakeLists.txt),
// exactly as it does for an in-tree consumer. Checked with std::to_underlying
// (library, added in C++23) rather than __cplusplus: GCC's __cplusplus for
// -std=c++23 was 202100L, not the standard's own 202302L, until later GCC
// releases caught up, which makes the macro's numeric value compiler-version
// trivia, not a reliable "did C++23 arrive" signal. If install(EXPORT) ever
// stopped carrying cxx_std_23, the compiler would fall back to its default
// standard and std::to_underlying would not exist to call.
static_assert(std::to_underlying(sub0log::Severity::Info)
                  == static_cast<std::underlying_type_t<sub0log::Severity>>(
                         sub0log::Severity::Info),
              "std::to_underlying (C++23, <utility>) is unavailable -- C++23 "
              "did not arrive as a usage requirement of the installed "
              "Sub0Log::Sub0Log target");

int main()
{
    // Both spellings version.hpp exports -- the integer macro and the
    // string function -- must agree, proving the installed header is
    // internally consistent rather than merely present.
    static_assert(SUB0LOG_VERSION == SUB0LOG_VERSION_MAJOR * 10000 +
                                      SUB0LOG_VERSION_MINOR * 100 +
                                      SUB0LOG_VERSION_PATCH);
    if (sub0log::libraryVersion().empty()) {
        std::fprintf(stderr, "sub0log::libraryVersion() returned empty\n");
        return 1;
    }

    const auto dir = std::filesystem::temp_directory_path() / "sub0log-packaging-consumer";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "packaging"});
    if (!logger.valid()) {
        std::fprintf(stderr, "Logger::create failed against the installed package\n");
        return 1;
    }

    {
        sub0log::Logger::ScopedBind bind{logger};
        // This call site is also the /Zc:preprocessor check on MSVC: the
        // macro expands through __VA_OPT__, which the legacy MSVC
        // preprocessor mangles into a syntax error. If install(EXPORT)
        // ever stopped carrying that INTERFACE compile option, this line
        // is where a Windows build of this consumer would fail to
        // compile, not something inspection of CMakeLists.txt would catch.
        sub0log_info(cConsumer, "installed Sub0Log {} works", sub0log::libraryVersion());
    }

    std::filesystem::remove_all(dir, ec);
    std::printf("packaging test OK: Sub0Log %.*s\n",
                static_cast<int>(sub0log::libraryVersion().size()),
                sub0log::libraryVersion().data());
    return 0;
}
