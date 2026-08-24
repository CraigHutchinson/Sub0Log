#pragma once

/** @file detail/platform.hpp
 *  @brief The one platform interface (R8.1): a shared mapping over a real
 *         file, a machine-wide monotonic clock, and process/thread identity.
 *
 *  Everything platform-specific in the library lives behind this header.
 *
 *  The mapping rules that code cannot show (docs/hard-kill.md):
 *   - POSIX: MAP_SHARED over a real descriptor. MAP_PRIVATE is copy-on-write
 *     process memory and loses everything on a kill; it must never appear.
 *   - Windows: CreateFileMapping over a real file handle, FILE_MAP_WRITE. A
 *     section over INVALID_HANDLE_VALUE is pagefile-backed and has none of
 *     the durability this library exists for; it must never appear.
 *   - A test asserts the mapping has a real backing file, because this is
 *     the kind of property that decays as a comment.
 *
 *  The clock rule (R5.3): monotonicNowNs() must be comparable across
 *  processes on one machine -- CLOCK_MONOTONIC on Linux, QPC on Windows,
 *  CLOCK_MONOTONIC_RAW on macOS -- and is only ever interpreted through the
 *  segment's anchor pair, never compared to std::chrono::steady_clock.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace sub0log::detail {

/// Why a platform operation failed; carried upward, never thrown on the
/// producer hot path. (std::expected is deliberately absent: libstdc++ 13
/// withholds <expected> from Clang < 19, and that pairing is first-class.
/// Fallible constructors return their object with valid() false and this
/// stored beside it.)
struct PlatformError {
    int code_{};              ///< errno / GetLastError value.
    std::string_view what_{}; ///< Static description of the failing step.

    /// True when an error is actually present.
    [[nodiscard]] explicit operator bool() const noexcept { return !what_.empty(); }
};

/** A writable shared mapping over a real file, created at full size.
 *  Move-only RAII; unmapping does not lose written pages (that is the whole
 *  point). TODO(impl:producer): POSIX implementation (open/ftruncate/mmap
 *  with MAP_SHARED); Windows implementation behind #ifdef _WIN32, compiled
 *  but CI-unverified in v1.
 */
class FileMapping {
public:
    FileMapping() noexcept = default;
    FileMapping(FileMapping&& other) noexcept;
    FileMapping& operator=(FileMapping&& other) noexcept;
    FileMapping(const FileMapping&) = delete;
    FileMapping& operator=(const FileMapping&) = delete;
    ~FileMapping();

    /// Creates (exclusive) and sizes the file, then maps it shared. On
    /// failure the result has valid() false and error() set.
    [[nodiscard]] static FileMapping create(const std::string& path,
                                            std::uint64_t bytes) noexcept;

    /// Opens an existing segment read-only (the reader's path).
    [[nodiscard]] static FileMapping openReadOnly(const std::string& path) noexcept;

    [[nodiscard]] std::span<std::byte> bytes() const noexcept
    {
        return {static_cast<std::byte*>(base_), size_};
    }

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    [[nodiscard]] PlatformError error() const noexcept { return error_; }

private:
    void* base_{nullptr};
    std::size_t size_{0};
    std::intptr_t file_{-1}; ///< fd (POSIX) / HANDLE (Windows) for the real file.
    PlatformError error_{};
};

/// Machine-wide monotonic reading in nanoseconds (R5.3). Never
/// std::chrono::steady_clock: its epoch is unspecified per process.
[[nodiscard]] std::uint64_t monotonicNowNs() noexcept;

/// Wall clock in nanoseconds since the Unix epoch, for the anchor pair.
[[nodiscard]] std::uint64_t wallNowNs() noexcept;

[[nodiscard]] std::uint64_t currentProcessId() noexcept;
[[nodiscard]] std::uint64_t currentThreadId() noexcept;

/// Fills the segment generation: random, non-zero (R3.4).
[[nodiscard]] std::uint64_t randomGeneration() noexcept;

} // namespace sub0log::detail
