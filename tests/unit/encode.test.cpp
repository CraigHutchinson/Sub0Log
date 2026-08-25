// encode.hpp: the compile-time type mapping and the payload encoder. Byte
// layouts are checked by hand against wire.hpp's contract, not round-tripped
// through a decoder (that's the reader agent's territory).

#include <sub0log/encode.hpp>

#include "support/test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace sub0log;

namespace {
enum class Color : std::uint8_t { Red, Green, Blue };

/// Stands in for std::pmr::string and any third-party string type: the only
/// thing StringViewLike asks of an argument is this conversion, not that it
/// be a standard type.
struct CustomString {
    std::string data_;
    [[nodiscard]] operator std::string_view() const noexcept { return data_; }
};
} // namespace

// docs/adoption-friction.md 1.2: a std::string passed by const reference and
// inlined as bytes allocates nothing, so it takes the same Bytes path as
// std::string_view. This is checked as a concept acceptance, not by trying
// to compile an emit call.
static_assert(detail::Encodable<std::string>,
             "std::string must satisfy Encodable -- inlining its bytes by "
             "const reference allocates nothing (docs/adoption-friction.md "
             "1.2)");
static_assert(detail::Encodable<CustomString>,
             "a type with a non-throwing operator std::string_view() const "
             "must satisfy Encodable -- that is the generalisation this "
             "concept exists for");

// The three refusals docs/adoption-friction.md 1.2 says must stay: each
// needs a representation decision (contiguous element encoding, which of
// several string forms, a time unit) that only the call site can make, and
// none of them is convertible to std::string_view.
static_assert(!detail::Encodable<std::vector<int>>,
             "std::vector must not satisfy Encodable -- it needs an "
             "explicit encoding decision, not an implicit one");
static_assert(!detail::Encodable<std::filesystem::path>,
             "std::filesystem::path must not satisfy Encodable -- POSIX "
             "libstdc++'s operator string_type() converts to std::string, "
             "not std::string_view, and chaining that through std::string's "
             "own conversion would need two user-defined conversions in one "
             "sequence, which the language does not allow");
static_assert(!detail::Encodable<std::chrono::milliseconds>,
             "std::chrono::milliseconds must not satisfy Encodable -- the "
             "call site must choose a unit explicitly (e.g. .count())");

TEST_CASE("typeCodeFor maps representative types to their wire codes")
{
    CHECK(detail::typeCodeFor<bool>() == wire::TypeCode::Bool);
    CHECK(detail::typeCodeFor<char>() == wire::TypeCode::Char);
    CHECK(detail::typeCodeFor<std::int8_t>() == wire::TypeCode::I8);
    CHECK(detail::typeCodeFor<std::uint8_t>() == wire::TypeCode::U8);
    CHECK(detail::typeCodeFor<std::int16_t>() == wire::TypeCode::I16);
    CHECK(detail::typeCodeFor<std::uint16_t>() == wire::TypeCode::U16);
    CHECK(detail::typeCodeFor<std::int32_t>() == wire::TypeCode::I32);
    CHECK(detail::typeCodeFor<std::uint32_t>() == wire::TypeCode::U32);
    CHECK(detail::typeCodeFor<std::int64_t>() == wire::TypeCode::I64);
    CHECK(detail::typeCodeFor<std::uint64_t>() == wire::TypeCode::U64);
    CHECK(detail::typeCodeFor<float>() == wire::TypeCode::F32);
    CHECK(detail::typeCodeFor<double>() == wire::TypeCode::F64);
    CHECK(detail::typeCodeFor<void*>() == wire::TypeCode::Pointer);
    CHECK(detail::typeCodeFor<const int*>() == wire::TypeCode::Pointer);
    CHECK(detail::typeCodeFor<std::string_view>() == wire::TypeCode::Bytes);
    CHECK(detail::typeCodeFor<std::span<const std::byte>>() == wire::TypeCode::Bytes);
    CHECK(detail::typeCodeFor<const char*>() == wire::TypeCode::Bytes);
    CHECK(detail::typeCodeFor<std::string>() == wire::TypeCode::Bytes);
    CHECK(detail::typeCodeFor<CustomString>() == wire::TypeCode::Bytes);
    CHECK(detail::typeCodeFor<Color>() == wire::TypeCode::U8); // underlying type
}

TEST_CASE("encodeArgs packs fixed-size arguments little-endian")
{
    alignas(8) std::byte buf[64]{};
    const std::uint32_t needed = detail::upperBoundSize(std::int32_t{-7}, std::uint16_t{300}, true);
    REQUIRE(needed == 4u + 2u + 1u);

    const auto result =
        detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), std::int32_t{-7},
                           std::uint16_t{300}, true);
    CHECK(result.bytes_ == needed);
    CHECK_FALSE(result.truncated_);

    // int32_t(-7) == 0xFFFFFFF9, little-endian.
    CHECK(buf[0] == std::byte{0xF9});
    CHECK(buf[1] == std::byte{0xFF});
    CHECK(buf[2] == std::byte{0xFF});
    CHECK(buf[3] == std::byte{0xFF});
    // uint16_t(300) == 0x012C, little-endian.
    CHECK(buf[4] == std::byte{0x2C});
    CHECK(buf[5] == std::byte{0x01});
    // bool true is exactly one byte, value 1 (never sizeof(bool)).
    CHECK(buf[6] == std::byte{0x01});
}

TEST_CASE("encodeArgs stores an enum as its underlying type")
{
    alignas(8) std::byte buf[8]{};
    const auto result = detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), Color::Green);
    CHECK(result.bytes_ == 1u);
    CHECK(buf[0] == std::byte{1}); // Color::Green == 1
}

TEST_CASE("encodeArgs packs a string_view as a u16 length then bytes")
{
    alignas(8) std::byte buf[64]{};
    const std::string_view text = "hi";
    const auto result = detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), text);

    CHECK(result.bytes_ == 2u + text.size());
    CHECK_FALSE(result.truncated_);
    CHECK(buf[0] == std::byte{0x02}); // u16 length, low byte
    CHECK(buf[1] == std::byte{0x00}); // u16 length, high byte
    CHECK(buf[2] == std::byte{'h'});
    CHECK(buf[3] == std::byte{'i'});
}

TEST_CASE("encodeArgs packs a span<const byte> the same way as a string_view")
{
    const std::byte raw[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
    alignas(8) std::byte buf[64]{};
    const auto result =
        detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), std::span<const std::byte>(raw));

    CHECK(result.bytes_ == 2u + 3u);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf) == 3u);
    CHECK(buf[2] == std::byte{1});
    CHECK(buf[3] == std::byte{2});
    CHECK(buf[4] == std::byte{3});
}

TEST_CASE("a const char* argument is measured once by strlen; null is length 0")
{
    alignas(8) std::byte buf[64]{};
    const char* const text = "abc";
    const auto result = detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), text);
    CHECK(result.bytes_ == 2u + 3u);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf) == 3u);
    CHECK(buf[2] == std::byte{'a'});

    const char* const nullText = nullptr;
    const auto nullResult = detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), nullText);
    CHECK(nullResult.bytes_ == 2u);
    CHECK_FALSE(nullResult.truncated_);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf) == 0u);
}

// docs/adoption-friction.md 1.2: the whole point of accepting std::string is
// that it produces the identical bytes std::string_view{s} already did, for
// an lvalue, a const lvalue and a temporary alike -- an argument's lifetime
// covers the full expression it appears in, temporaries included, so there
// is nothing for the temporary case to dangle into by the time encodeArgs
// runs.
TEST_CASE("a std::string lvalue, const lvalue and temporary all encode to the "
         "same bytes as the equivalent std::string_view")
{
    const std::string_view expected = "hello, sub0log";

    alignas(8) std::byte viaView[64]{};
    const auto viewResult =
        detail::encodeArgs(viaView, static_cast<std::uint32_t>(sizeof(viaView)), expected);

    std::string mutableCopy = "hello, sub0log";
    alignas(8) std::byte viaLvalue[64]{};
    const auto lvalueResult =
        detail::encodeArgs(viaLvalue, static_cast<std::uint32_t>(sizeof(viaLvalue)), mutableCopy);
    CHECK(lvalueResult.bytes_ == viewResult.bytes_);
    CHECK(std::memcmp(viaLvalue, viaView, lvalueResult.bytes_) == 0);

    const std::string constCopy = "hello, sub0log";
    alignas(8) std::byte viaConstLvalue[64]{};
    const auto constLvalueResult = detail::encodeArgs(
        viaConstLvalue, static_cast<std::uint32_t>(sizeof(viaConstLvalue)), constCopy);
    CHECK(constLvalueResult.bytes_ == viewResult.bytes_);
    CHECK(std::memcmp(viaConstLvalue, viaView, constLvalueResult.bytes_) == 0);

    alignas(8) std::byte viaTemporary[64]{};
    const auto temporaryResult = detail::encodeArgs(
        viaTemporary, static_cast<std::uint32_t>(sizeof(viaTemporary)), std::string("hello, sub0log"));
    CHECK(temporaryResult.bytes_ == viewResult.bytes_);
    CHECK(std::memcmp(viaTemporary, viaView, temporaryResult.bytes_) == 0);
}

// The generalisation StringViewLike exists for: nothing here is std::string
// or std::string_view, only a type that offers the conversion -- exactly
// what std::pmr::string and a third-party string type would offer.
TEST_CASE("a user-defined type with operator std::string_view() const encodes "
         "the same way std::string does")
{
    const CustomString custom{"third-party string"};
    const std::string_view expected = "third-party string";

    alignas(8) std::byte viaCustom[64]{};
    const auto customResult =
        detail::encodeArgs(viaCustom, static_cast<std::uint32_t>(sizeof(viaCustom)), custom);

    alignas(8) std::byte viaView[64]{};
    const auto viewResult =
        detail::encodeArgs(viaView, static_cast<std::uint32_t>(sizeof(viaView)), expected);

    CHECK(customResult.bytes_ == viewResult.bytes_);
    CHECK_FALSE(customResult.truncated_);
    CHECK(std::memcmp(viaCustom, viaView, customResult.bytes_) == 0);
}

TEST_CASE("an empty std::string encodes as a zero length with no bytes following")
{
    alignas(8) std::byte buf[64]{};
    const std::string empty{};
    const auto result = detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), empty);

    CHECK(result.bytes_ == 2u);
    CHECK_FALSE(result.truncated_);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf) == 0u);
}

TEST_CASE("encodeArgs truncates a std::string at cInlineBytesCap and flags it, "
         "exactly like a string_view (R9.2: a cut is visible, never silent)")
{
    const std::string big(static_cast<std::size_t>(wire::cInlineBytesCap) + 100u, 'x');

    CHECK(detail::upperBoundSize(big) == 2u + wire::cInlineBytesCap);

    std::vector<std::byte> buf(2u + wire::cInlineBytesCap + 16u);
    const auto result = detail::encodeArgs(buf.data(), static_cast<std::uint32_t>(buf.size()), big);

    CHECK(result.truncated_);
    CHECK(result.bytes_ == 2u + wire::cInlineBytesCap);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf.data()) == wire::cInlineBytesCap);
}

TEST_CASE("encodeArgs truncates a view at cInlineBytesCap and flags it")
{
    const std::vector<char> big(static_cast<std::size_t>(wire::cInlineBytesCap) + 100u, 'x');
    const std::string_view text(big.data(), big.size());

    CHECK(detail::upperBoundSize(text) == 2u + wire::cInlineBytesCap);

    std::vector<std::byte> buf(2u + wire::cInlineBytesCap + 16u);
    const auto result = detail::encodeArgs(buf.data(), static_cast<std::uint32_t>(buf.size()), text);

    CHECK(result.truncated_);
    CHECK(result.bytes_ == 2u + wire::cInlineBytesCap);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf.data()) == wire::cInlineBytesCap);
}

TEST_CASE("encodeArgs round-trips a mix of fixed and variable arguments")
{
    alignas(8) std::byte buf[128]{};
    const std::string_view name = "blob";
    const auto result =
        detail::encodeArgs(buf, static_cast<std::uint32_t>(sizeof(buf)), std::uint64_t{0xDEADBEEFu}, name,
                           false);

    const auto expected = static_cast<std::uint32_t>(8u + (2u + name.size()) + 1u);
    CHECK(result.bytes_ == expected);
    CHECK(detail::upperBoundSize(std::uint64_t{0xDEADBEEFu}, name, false) == expected);

    CHECK(wire::loadUnaligned<std::uint64_t>(buf) == 0xDEADBEEFu);
    CHECK(wire::loadUnaligned<std::uint16_t>(buf + 8) == name.size());
    CHECK(buf[10] == std::byte{'b'});
    CHECK(buf[8 + 2 + name.size()] == std::byte{0}); // trailing bool false
}

// A type whose encoded size and whose wire type code disagree corrupts every
// argument after it in the record: the encoder writes sizeof(long double)
// bytes, the decoder reads the 8 that F64 means, and the rest of the payload
// shifts. The wire has no code for it, so the only place to catch it is here.
static_assert(!detail::Encodable<long double>,
              "long double must not satisfy Encodable: it maps to F64 but is "
              "not 8 bytes, which would silently desync the record");

// The general rule the above is one instance of: for every fixed-size type
// the encoder accepts, the bytes it writes must equal the bytes the decoder
// consumes for that type's code.
static_assert(detail::WireSizeAgrees<std::uint64_t>);
static_assert(detail::WireSizeAgrees<std::int32_t>);
static_assert(detail::WireSizeAgrees<double>);
static_assert(detail::WireSizeAgrees<float>);
static_assert(detail::WireSizeAgrees<bool>);
static_assert(detail::WireSizeAgrees<char>);
static_assert(detail::WireSizeAgrees<void*>);
