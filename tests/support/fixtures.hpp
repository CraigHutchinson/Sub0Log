#pragma once

/** @file tests/support/fixtures.hpp
 *  @brief Filesystem fixtures shared by integration and system tests.
 *
 *  Doctest assertions inside helpers are deliberate: a fixture that cannot
 *  set up fails the test that asked for it, at the line that asked.
 */

#include <sub0log/detail/platform.hpp>

#include "test_framework.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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

/** A std::filesystem::path from UTF-8 bytes, said unambiguously.
 *
 *  `path{std::string}` interprets its argument in the platform's *narrow*
 *  encoding -- the active code page on Windows, which is not UTF-8 unless
 *  somebody asked for it -- so a non-ASCII path put through it comes back
 *  as mojibake or fails outright. The u8string overload is the one that
 *  means "these bytes are UTF-8", which is what every std::string path in
 *  this library is (Logger::Options::directory_, Logger::segmentPath()).
 *  Tests that touch a non-ASCII path must go through here, or they test
 *  the code page rather than the library.
 */
inline std::filesystem::path pathFromUtf8(const std::string_view utf8)
{
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
}

/// freshDirectory(), with a non-ASCII component in the name, returned as
/// the UTF-8 std::string the library takes. The directory itself is created
/// through pathFromUtf8() so the bytes on disk are what the caller wrote.
inline std::string freshUtf8Directory(const char* const tag, const std::string_view nameUtf8)
{
    // u8string() rather than string() for the temp path too: on a machine
    // whose own user name is outside the code page, string() would already
    // have lost before this test's own non-ASCII component was appended.
    const std::u8string tempUtf8 = std::filesystem::temp_directory_path().u8string();
    std::string utf8{reinterpret_cast<const char*>(tempUtf8.data()), tempUtf8.size()};
    if (!utf8.empty() && utf8.back() != '/' && utf8.back() != '\\') {
        utf8 += '/';
    }
    utf8 += "sub0log-t-";
    utf8 += tag;
    utf8 += '-';
    utf8 += std::to_string(detail::currentProcessId());
    utf8 += '-';
    utf8 += nameUtf8;
    const auto path = pathFromUtf8(utf8);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return utf8;
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

/// setenv/unsetenv are POSIX; MSVC has _putenv_s, where an empty value is
/// how a variable is removed. Tests say what they mean and this pair
/// absorbs the difference.
inline void setEnvVar(const char* const name, const char* const value)
{
#if defined(_WIN32)
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

inline void unsetEnvVar(const char* const name)
{
#if defined(_WIN32)
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
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
