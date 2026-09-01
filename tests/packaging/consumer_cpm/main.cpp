// The whole point of this file: it must compile and link against a Sub0Log
// that CMakeLists.txt in this directory fetched with CPMAddPackage(), not
// against this repository's own include/ tree directly. If Sub0Log::Sub0Log
// stopped being the name CPM's add_subdirectory() path produces, or stopped
// carrying its usage requirements (C++23, /Zc:preprocessor on MSVC) that
// same way find_package()'s exported target does, this consumer (driven by
// tests/packaging/drive_cpm.cmake) is where that shows up as a hard build
// failure rather than a README example nobody tried.

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

// See tests/packaging/consumer/main.cpp for why std::to_underlying rather
// than __cplusplus: the same usage-requirement is under test here, just
// arriving via CPM's add_subdirectory() instead of an installed package's
// find_package().
static_assert(std::to_underlying(sub0log::Severity::Info)
                  == static_cast<std::underlying_type_t<sub0log::Severity>>(
                         sub0log::Severity::Info),
              "std::to_underlying (C++23, <utility>) is unavailable -- C++23 "
              "did not arrive as a usage requirement of the CPM-fetched "
              "Sub0Log::Sub0Log target");

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / "sub0log-cpm-consumer";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "cpm"});
    if (!logger.valid()) {
        std::fprintf(stderr, "Logger::create failed against the CPM-fetched package\n");
        return 1;
    }

    {
        sub0log::Logger::ScopedBind bind{logger};
        // Also the /Zc:preprocessor check on MSVC, same reasoning as
        // tests/packaging/consumer/main.cpp: this macro expands through
        // __VA_OPT__, which the legacy MSVC preprocessor mangles.
        sub0log_info(cConsumer, "CPM-fetched Sub0Log {} works", sub0log::libraryVersion());
    }

    std::filesystem::remove_all(dir, ec);
    std::printf("cpm packaging test OK: Sub0Log %.*s\n",
                static_cast<int>(sub0log::libraryVersion().size()),
                sub0log::libraryVersion().data());
    return 0;
}
