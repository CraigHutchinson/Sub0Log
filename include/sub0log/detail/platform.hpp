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

#include <cerrno>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <pthread.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
#endif

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
 *  point).
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
    /// Releases whatever this instance currently owns and resets to the
    /// empty state. Shared by the destructor and move-assignment so the two
    /// don't drift apart.
    void releaseUnlocked() noexcept;

    void* base_{nullptr};
    std::size_t size_{0};
    std::intptr_t file_{-1}; ///< fd (POSIX) / HANDLE (Windows) for the real file.
#if defined(_WIN32)
    std::intptr_t mappingHandle_{-1}; ///< HANDLE from CreateFileMappingW.
#endif
    PlatformError error_{};
};

// ---------------------------------------------------------------------------
// FileMapping implementation

inline void FileMapping::releaseUnlocked() noexcept
{
#if defined(_WIN32)
    if (base_ != nullptr) {
        ::UnmapViewOfFile(base_);
    }
    if (mappingHandle_ != -1) {
        ::CloseHandle(reinterpret_cast<HANDLE>(mappingHandle_));
        mappingHandle_ = -1;
    }
    if (file_ != -1) {
        ::CloseHandle(reinterpret_cast<HANDLE>(file_));
    }
#else
    if (base_ != nullptr) {
        ::munmap(base_, size_);
    }
    if (file_ != -1) {
        ::close(static_cast<int>(file_));
    }
#endif
    base_ = nullptr;
    size_ = 0;
    file_ = -1;
}

inline FileMapping::FileMapping(FileMapping&& other) noexcept
    : base_{other.base_}, size_{other.size_}, file_{other.file_},
#if defined(_WIN32)
      mappingHandle_{other.mappingHandle_},
#endif
      error_{other.error_}
{
    other.base_ = nullptr;
    other.size_ = 0;
    other.file_ = -1;
#if defined(_WIN32)
    other.mappingHandle_ = -1;
#endif
}

inline FileMapping& FileMapping::operator=(FileMapping&& other) noexcept
{
    if (this != &other) {
        releaseUnlocked();
        base_ = other.base_;
        size_ = other.size_;
        file_ = other.file_;
#if defined(_WIN32)
        mappingHandle_ = other.mappingHandle_;
        other.mappingHandle_ = -1;
#endif
        error_ = other.error_;
        other.base_ = nullptr;
        other.size_ = 0;
        other.file_ = -1;
    }
    return *this;
}

inline FileMapping::~FileMapping()
{
    releaseUnlocked();
}

inline FileMapping FileMapping::create(const std::string& path, std::uint64_t bytes) noexcept
{
    FileMapping result{};
#if defined(_WIN32)
    // Sharing is the point, not a concession: a reader tails a live segment
    // while its producer is still writing (docs/record-model.md, "the console
    // view is a view"), and a merge reads segments of processes that are still
    // running. CREATE_NEW is what guarantees this call is the creator; the
    // share mode was never what protected that, and a restrictive one here
    // silently forbids every reader. POSIX imposes no equivalent restriction,
    // which is why only the Windows job could show this.
    const HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "CreateFileA"};
        return result;
    }
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(bytes);
    if (!::SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !::SetEndOfFile(file)) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "SetEndOfFile"};
        ::CloseHandle(file);
        return result;
    }
    // Over a real file handle, never INVALID_HANDLE_VALUE (that would be
    // pagefile-backed and lose everything -- docs/hard-kill.md).
    const HANDLE mapping = ::CreateFileMappingW(
        file, nullptr, PAGE_READWRITE, static_cast<DWORD>(bytes >> 32u),
        static_cast<DWORD>(bytes & 0xFFFFFFFFu), nullptr);
    if (mapping == nullptr) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "CreateFileMappingW"};
        ::CloseHandle(file);
        return result;
    }
    void* const base = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
    if (base == nullptr) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "MapViewOfFile"};
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        return result;
    }
    result.base_ = base;
    result.size_ = static_cast<std::size_t>(bytes);
    result.file_ = reinterpret_cast<std::intptr_t>(file);
    result.mappingHandle_ = reinterpret_cast<std::intptr_t>(mapping);
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) {
        result.error_ = PlatformError{errno, "open"};
        return result;
    }
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        result.error_ = PlatformError{errno, "ftruncate"};
        ::close(fd);
        return result;
    }
    // MAP_SHARED over the real fd -- MAP_PRIVATE must never appear here
    // (docs/hard-kill.md: it is copy-on-write and loses everything on a
    // kill of the mapping process).
    void* const base = ::mmap(nullptr, static_cast<std::size_t>(bytes),
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        result.error_ = PlatformError{errno, "mmap"};
        ::close(fd);
        return result;
    }
    result.base_ = base;
    result.size_ = static_cast<std::size_t>(bytes);
    result.file_ = fd;
#endif
    return result;
}

inline FileMapping FileMapping::openReadOnly(const std::string& path) noexcept
{
    FileMapping result{};
#if defined(_WIN32)
    // FILE_SHARE_WRITE is required on the *reader's* side too: Windows checks
    // the opener's share mode against existing handles' access, so a reader
    // that does not permit writing cannot open a segment a producer still has
    // open for writing -- which is exactly the live-tail case.
    const HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "CreateFileA"};
        return result;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "GetFileSizeEx"};
        ::CloseHandle(file);
        return result;
    }
    const HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "CreateFileMappingW"};
        ::CloseHandle(file);
        return result;
    }
    void* const base = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (base == nullptr) {
        result.error_ = PlatformError{static_cast<int>(::GetLastError()), "MapViewOfFile"};
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        return result;
    }
    result.base_ = base;
    result.size_ = static_cast<std::size_t>(size.QuadPart);
    result.file_ = reinterpret_cast<std::intptr_t>(file);
    result.mappingHandle_ = reinterpret_cast<std::intptr_t>(mapping);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        result.error_ = PlatformError{errno, "open"};
        return result;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        result.error_ = PlatformError{errno, "fstat"};
        ::close(fd);
        return result;
    }
    const std::size_t bytes = static_cast<std::size_t>(st.st_size);
    void* const base = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        result.error_ = PlatformError{errno, "mmap"};
        ::close(fd);
        return result;
    }
    result.base_ = base;
    result.size_ = bytes;
    result.file_ = fd;
#endif
    return result;
}

// ---------------------------------------------------------------------------
// Clock and identity

/// Machine-wide monotonic reading in nanoseconds (R5.3). Never
/// std::chrono::steady_clock: its epoch is unspecified per process.
[[nodiscard]] std::uint64_t monotonicNowNs() noexcept;

/// Wall clock in nanoseconds since the Unix epoch, for the anchor pair.
[[nodiscard]] std::uint64_t wallNowNs() noexcept;

[[nodiscard]] std::uint64_t currentProcessId() noexcept;
[[nodiscard]] std::uint64_t currentThreadId() noexcept;

/// Fills the segment generation: random, non-zero (R3.4).
[[nodiscard]] std::uint64_t randomGeneration() noexcept;

#if defined(_WIN32)

[[nodiscard]] inline std::uint64_t monotonicNowNs() noexcept
{
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    // Split whole/fractional to avoid overflow scaling to nanoseconds.
    const std::uint64_t whole = (ticks / frequency) * 1'000'000'000ull;
    const std::uint64_t frac = (ticks % frequency) * 1'000'000'000ull / frequency;
    return whole + frac;
}

[[nodiscard]] inline std::uint64_t wallNowNs() noexcept
{
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // FILETIME: 100ns intervals since 1601-01-01. Convert to ns since the
    // Unix epoch (1970-01-01); the constant is the interval count between
    // the two epochs.
    constexpr std::uint64_t cEpochDiff100Ns = 116444736000000000ull;
    const std::uint64_t hundredNs = uli.QuadPart - cEpochDiff100Ns;
    return hundredNs * 100ull;
}

[[nodiscard]] inline std::uint64_t currentProcessId() noexcept
{
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

[[nodiscard]] inline std::uint64_t currentThreadId() noexcept
{
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
}

#else // POSIX

[[nodiscard]] inline std::uint64_t monotonicNowNs() noexcept
{
    struct timespec ts {};
#  if defined(__APPLE__)
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#  else
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
#  endif
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

[[nodiscard]] inline std::uint64_t wallNowNs() noexcept
{
    struct timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

[[nodiscard]] inline std::uint64_t currentProcessId() noexcept
{
    return static_cast<std::uint64_t>(::getpid());
}

[[nodiscard]] inline std::uint64_t currentThreadId() noexcept
{
#  if defined(__APPLE__)
    std::uint64_t tid = 0;
    ::pthread_threadid_np(nullptr, &tid);
    return tid;
#  elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#  else
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pthread_self()));
#  endif
}

#endif // platform switch

[[nodiscard]] inline std::uint64_t randomGeneration() noexcept
{
    // Deliberately not std::random_device, for two reasons that both matter
    // to whoever includes this header.
    //
    // It can throw when its source is unavailable, and a try/catch here made
    // the entire producer header chain refuse to compile under
    // -fno-exceptions -- a build mode any memory-restricted or freestanding
    // target is likely to use, for a header that must not need exceptions to
    // copy bytes into a mapping.
    //
    // And it costs: <random> was 59,000 of log.hpp's 71,000 preprocessed
    // lines, pulled in so one call per process could season a number.
    //
    // What this value must actually be is unique per segment with high
    // probability, so a stale chunk from a previous run is rejected on
    // positive evidence (R3.4). It is not a security value and nothing
    // depends on it being unpredictable. Mixing the two clocks, the process
    // id and this call's own stack address through splitmix64 gives that,
    // for the cost of a few multiplies.
    std::uint64_t value = 0;
    const auto stackNoise = reinterpret_cast<std::uintptr_t>(&value);
    value ^= static_cast<std::uint64_t>(stackNoise) * 0xD6E8FEB86659FD93ull;
    value ^= monotonicNowNs() * 0x9E3779B97F4A7C15ull;
    value ^= (currentProcessId() + 1u) * 0xBF58476D1CE4E5B9ull;
    value ^= wallNowNs() * 0x94D049BB133111EBull;
    if (value == 0u) {
        // Astronomically unlikely, but the contract is non-zero (R3.4).
        value = 0x9E3779B97F4A7C15ull;
    }
    return value;
}

} // namespace sub0log::detail
