// detail/platform.hpp: detail::toWidePath, the UTF-8 -> UTF-16 path
// conversion behind the `*W` Win32 calls (docs/adoption-friction.md 4.1).
//
// Windows-only: toWidePath calls MultiByteToWideChar directly and exists
// only in platform.hpp's `_WIN32` arm, so this whole file compiles to an
// empty translation unit on POSIX. It could not be made portable without
// either linking a UTF-8/UTF-16 conversion library the producer path does
// not otherwise need, or reimplementing MultiByteToWideChar's behaviour in
// test-only code -- neither is worth doing to test a handful of lines that
// are Win32-specific by nature. The windows-msvc CI job is what actually
// exercises this.

#include "support/test_framework.hpp"

#if defined(_WIN32)

#include <sub0log/detail/platform.hpp>

#include <string>

using namespace sub0log::detail;

TEST_CASE("toWidePath: empty path converts to an empty wide string, no error")
{
    const auto result = toWidePath(std::string{});
    CHECK_FALSE(result.error_);
    CHECK(result.value_.empty());
}

TEST_CASE("toWidePath: an ASCII path round-trips exactly")
{
    const auto result = toWidePath(std::string{"C:\\logs\\seg.bin"});
    CHECK_FALSE(result.error_);
    CHECK(result.value_ == L"C:\\logs\\seg.bin");
}

TEST_CASE("toWidePath: a non-ASCII UTF-8 path converts correctly")
{
    // U+00E9 (e-acute), UTF-8 encoded as 0xC3 0xA9 -- e.g. a user profile
    // directory outside the ASCII range (docs/adoption-friction.md 4.1).
    const std::string path = "C:\\Users\\caf\xC3\xA9\\logs\\seg.bin";
    const auto result = toWidePath(path);
    CHECK_FALSE(result.error_);
    CHECK(result.value_ == L"C:\\Users\\caf\u00E9\\logs\\seg.bin");
}

TEST_CASE("toWidePath: ill-formed UTF-8 fails classified, not mangled")
{
    // 0xFF is not a valid byte anywhere in UTF-8.
    const std::string path = "C:\\bad\xFF\\seg.bin";
    const auto result = toWidePath(path);
    CHECK(static_cast<bool>(result.error_));
    CHECK(result.value_.empty());
}

#endif // _WIN32
