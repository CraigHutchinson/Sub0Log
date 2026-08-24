#pragma once

/** @file support/temp_dir.hpp
 *  @brief Filesystem fixtures for the benchmark suite: a per-process
 *         temporary directory, cleaned up at exit, and slurp/find helpers
 *         mirroring tests/support/fixtures.hpp (no doctest here, so the
 *         assertions there become plain checks the caller acts on).
 */

#include <sub0log/detail/platform.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace sub0log::bench {

/** A fresh, empty temporary directory, removed on destruction. Cleared
 *  first (like tests/support/fixtures.hpp::freshDirectory), so a directory
 *  left behind by an aborted run is replaced rather than tripped over.
 */
class TempDir {
public:
    explicit TempDir(const std::string& tag)
        : path_{std::filesystem::temp_directory_path()
              / ("sub0log-bench-" + tag + "-"
                 + std::to_string(sub0log::detail::currentProcessId()))}
    {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code ec; // best-effort cleanup; nothing to do on failure
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// The whole file as bytes. Returns an empty vector if the file cannot be
/// opened -- callers check for that rather than an exception (setup code
/// here mirrors the noexcept discipline of the path under measurement).
[[nodiscard]] inline std::vector<std::byte> slurpFile(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        return {};
    }
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

/// The single .s0l segment a fixture-building helper expects to find; an
/// empty path if there is not exactly one.
[[nodiscard]] inline std::filesystem::path onlySegmentIn(const std::filesystem::path& directory)
{
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().extension() == ".s0l") {
            if (!found.empty()) {
                return {}; // more than one: ambiguous, treat as not-found
            }
            found = entry.path();
        }
    }
    return found;
}

} // namespace sub0log::bench
