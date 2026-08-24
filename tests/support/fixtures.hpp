#pragma once

/** @file tests/support/fixtures.hpp
 *  @brief Filesystem fixtures shared by integration and system tests.
 *
 *  Doctest assertions inside helpers are deliberate: a fixture that cannot
 *  set up fails the test that asked for it, at the line that asked.
 */

#include <sub0log/detail/platform.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sub0log::test {

/// A fresh, empty per-process temp directory; remove it at the end of the
/// test (a leaked directory from an aborted run is replaced, not tripped
/// over, because the same tag+pid path is cleared first).
inline std::filesystem::path freshDirectory(const char* const tag)
{
    auto path = std::filesystem::temp_directory_path()
              / (std::string{"sub0log-t-"} + tag + "-"
                 + std::to_string(detail::currentProcessId()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

/// The whole file as bytes.
inline std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    std::vector<char> raw{std::istreambuf_iterator<char>{in},
                          std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

/// The single .s0l segment a test expects its Logger to have produced;
/// fails the test if there are zero or several.
inline std::filesystem::path onlySegmentIn(const std::filesystem::path& directory)
{
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().extension() == ".s0l") {
            REQUIRE(found.empty());
            found = entry.path();
        }
    }
    REQUIRE(!found.empty());
    return found;
}

} // namespace sub0log::test
