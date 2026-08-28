#pragma once

/** @file encode.hpp
 *  @brief Compile-time mapping from C++ argument types to wire type codes,
 *         and the payload encoder.
 *
 *  The refusal is the point (docs/record-model.md): an argument that is not
 *  trivially copyable, or that would silently convert into an owning type,
 *  does not compile. There is no formatting fallback anywhere in here.
 */

#include "wire.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sub0log::detail {

// ---------------------------------------------------------------------------
// Type classification

template <typename T>
using Decayed = std::remove_cvref_t<T>;

template <typename T>
concept ByteView = std::same_as<Decayed<T>, std::string_view>
                || std::same_as<Decayed<T>, std::span<const std::byte>>;

// A const char* / char array argument is accepted and encoded as Bytes; it is
// measured once at the call, never copied into an owning string.
template <typename T>
concept CStringLike = std::same_as<std::decay_t<T>, const char*>
                   || std::same_as<std::decay_t<T>, char*>;

/// Anything whose `const&` converts to a `std::string_view`: `std::string`,
/// `std::pmr::string`, and any third-party string that offers
/// `operator string_view() const`. The conversion is a view over bytes the
/// argument already owns, taken once at the call site and read for the
/// lifetime of the full expression -- there is no allocation and no format
/// call anywhere in it, so it costs nothing that std::string_view{s} did not
/// already cost (docs/adoption-friction.md 1.2). ByteView and CStringLike are
/// excluded so a std::string_view or const char* argument keeps taking its
/// own, cheaper path instead of going through this one.
///
/// The conversion itself must be noexcept: encodeArgs and upperBoundSize are
/// noexcept end to end (R1.1-R1.3), and a throwing conversion buried inside
/// them would not fail to compile, it would call std::terminate the first
/// time it actually threw. std::string's operator string_view() is noexcept
/// in the standard; a third-party type that has not marked its own that way
/// is refused here rather than trusted at runtime.
template <typename T>
concept StringViewLike =
    !ByteView<T> && !CStringLike<T> &&
    std::is_nothrow_convertible_v<const Decayed<T>&, std::string_view>;

/// Every fixed-size type must have a wire size the decoder agrees on: the
/// encoder writes fixedWireSize<T>() bytes and the reader consumes
/// wire::fixedSizeOf(code). `long double` is the type that tests this, and
/// the answer is per-platform rather than per-type: it maps to F64 and is
/// 16 bytes on x86-64 SysV, so it would write 16 where the reader takes 8
/// and shift every argument after it -- but on AArch64 Darwin and on MSVC
/// it *is* binary64 and 8 bytes, where F64 is simply correct. The size
/// clause below therefore admits it exactly where the sizes agree, which is
/// the only place the mismatch can be caught since the wire has no room to
/// describe it.
template <typename T>
concept FixedEncodable = std::same_as<Decayed<T>, bool>
                      || std::same_as<Decayed<T>, char>
                      || (std::integral<Decayed<T>> && sizeof(Decayed<T>) <= 8)
                      || (std::floating_point<Decayed<T>>
                          && (sizeof(Decayed<T>) == 4 || sizeof(Decayed<T>) == 8))
                      || std::is_enum_v<Decayed<T>>
                      || std::is_pointer_v<Decayed<T>>;

/// The full set a call site may pass. Everything else is rejected at compile
/// time by typeCodeFor's static_assert, which names the two escape hatches.
template <typename T>
concept Encodable = FixedEncodable<T> || ByteView<T> || CStringLike<T> || StringViewLike<T>;

/// Maps a C++ type to its wire code. Instantiating this with an unsupported
/// type produces the library's canonical refusal message.
template <typename T>
[[nodiscard]] consteval wire::TypeCode typeCodeFor() noexcept
{
    using U = Decayed<T>;
    if constexpr (std::same_as<U, bool>) { return wire::TypeCode::Bool; }
    else if constexpr (std::same_as<U, char>) { return wire::TypeCode::Char; }
    else if constexpr (std::is_enum_v<U>) {
        return typeCodeFor<std::underlying_type_t<U>>();
    }
    else if constexpr (ByteView<T> || CStringLike<T> || StringViewLike<T>) {
        return wire::TypeCode::Bytes;
    }
    else if constexpr (std::is_pointer_v<U>) { return wire::TypeCode::Pointer; }
    else if constexpr (std::floating_point<U>) {
        return sizeof(U) == 4 ? wire::TypeCode::F32 : wire::TypeCode::F64;
    }
    else if constexpr (std::signed_integral<U>) {
        switch (sizeof(U)) {
        case 1: return wire::TypeCode::I8;
        case 2: return wire::TypeCode::I16;
        case 4: return wire::TypeCode::I32;
        default: return wire::TypeCode::I64;
        }
    }
    else if constexpr (std::unsigned_integral<U>) {
        switch (sizeof(U)) {
        case 1: return wire::TypeCode::U8;
        case 2: return wire::TypeCode::U16;
        case 4: return wire::TypeCode::U32;
        default: return wire::TypeCode::U64;
        }
    }
    else {
        static_assert(FixedEncodable<T> || ByteView<T> || CStringLike<T> || StringViewLike<T>,
                      "Sub0Log arguments must be trivially copyable values, "
                      "or something that views bytes it does not own or "
                      "format -- std::string_view, std::string and any type "
                      "with a non-throwing conversion to std::string_view all "
                      "take this path with no allocation. A std::vector, a "
                      "std::filesystem::path or a std::chrono::duration still "
                      "does not compile because each needs a representation "
                      "decision only the call site can make: log a stable "
                      "identifier instead, or convert explicitly (e.g. "
                      "path.native(), duration.count()) so that decision is "
                      "visible at the call site rather than hidden in the "
                      "encoder (docs/record-model.md).");
        return wire::TypeCode::Invalid;
    }
}

// ---------------------------------------------------------------------------
// Sizing and encoding
//
//  - upperBoundSize(args...): worst-case payload bytes for the argument pack
//    (fixed sizes + per-view u16 length + capped view bytes), used to decide
//    whether the record fits the chunk's remainder.
//  - encodeArgs(dst, capacity, args...): packs arguments little-endian:
//    fixed-size values raw via wire::storeUnaligned; views as u16 length then
//    bytes, truncated at wire::cInlineBytesCap. Returns bytes written and
//    whether anything was truncated (the caller sets cFlagTruncated -- a cut
//    is visible, never silent, R9.2).
//
//  No allocation, no formatting, no locks (R1.1-R1.3).

struct EncodeResult {
    std::uint32_t bytes_{};
    bool truncated_{};
};

/// Wire size of one fixed-size argument: bool is 1 byte, an enum is the size
/// of its underlying type, a pointer is always 8, everything else is its
/// own size (already true by construction of FixedEncodable).
///
/// A pointer is eight bytes on the wire whatever it is in the process,
/// which is a format decision rather than an encoding convenience. Records
/// are read by a separate tool, quite possibly a 64-bit one reading what a
/// 32-bit producer wrote, and TypeCode::Pointer has to mean one width for
/// that to work at all. Previously the encoder wrote sizeof(uintptr_t) and
/// FixedEncodable refused pointers wherever that was not 8 -- which kept
/// the mismatch out of the wire, at the price of a 32-bit target not being
/// able to log a pointer at all, with a diagnostic about trivially
/// copyable types that never mentions word size.
template <typename T>
[[nodiscard]] constexpr std::uint32_t fixedWireSize() noexcept
{
    using U = Decayed<T>;
    if constexpr (std::same_as<U, bool>) {
        return 1u;
    } else if constexpr (std::is_enum_v<U>) {
        return static_cast<std::uint32_t>(sizeof(std::underlying_type_t<U>));
    } else if constexpr (std::is_pointer_v<U>) {
        return 8u;
    } else {
        return static_cast<std::uint32_t>(sizeof(U));
    }
}

/// Length of a C string, never scanning past the cap. Sizing and encoding
/// both go through this: two independent scans could disagree (the string
/// is the caller's and may change between them), and an unbounded strlen on
/// the emit path costs whatever the caller's buffer happens to be, which
/// R1 has no interest in paying for bytes that will be truncated anyway.
[[nodiscard]] inline std::uint32_t boundedLength(const char* const ptr) noexcept
{
    if (ptr == nullptr) {
        return 0u;
    }
    std::uint32_t len = 0;
    while (len < wire::cInlineBytesCap && ptr[len] != '\0') {
        ++len;
    }
    return len;
}

// ---------------------------------------------------------------------------
// Continuation-chain sizing (detail/emit.hpp's chained path only)
//
// Nothing below this point is reached by a call site whose argument pack
// has no Bytes-shaped argument at all -- cHasByteArg is checked with
// `if constexpr` at the one call site that matters (emit()), so a pack of
// only fixed-size arguments never instantiates, let alone runs, any of it.

/// True when the pack contains at least one argument that encodes as
/// wire::TypeCode::Bytes. Decided at compile time so detail::emit can route
/// a call site with none of these through the original, unmodified path --
/// the branch this buys is a compiler decision, not a runtime one, which is
/// what keeps a fixed-only call site (emit.fixed2's shape) paying nothing
/// for continuation-chain support existing at all.
template <typename... Args>
inline constexpr bool cHasByteArg = (false || ... || (ByteView<Args> || CStringLike<Args> || StringViewLike<Args>));

/// Ceiling on one argument's total logical length that the chained emit
/// path will ever act on: the inline cap plus the most a full chain can
/// carry (wire::cMaxContinuations further records of wire::cInlineBytesCap
/// bytes each). Bytes past this are truncated, exactly as bytes past
/// wire::cInlineBytesCap alone always were -- this is that same cut, moved
/// out to the chain's ceiling instead of the first record's.
inline constexpr std::size_t cArgCeiling =
    static_cast<std::size_t>(wire::cInlineBytesCap)
    * (1u + static_cast<std::size_t>(wire::cMaxContinuations));

/// Raw pointer and length for one Bytes-producing argument; {nullptr, 0}
/// for anything else. "Raw" means uncapped for a view type (its size() is
/// already O(1), so there is no cost to knowing the true length) and capped
/// at cArgCeiling **plus one** for a CStringLike argument -- the one extra
/// byte is what tells the sizing pass in emitChained "this is longer than
/// the ceiling, not merely as long as it", without scanning any further to
/// learn it (the exact excess past the ceiling is truncated regardless, so
/// nothing needs it). Bounding the scan there rather than at cArgCeiling
/// itself is the whole difference between correctly flagging a
/// past-the-ceiling argument as truncated and, as an earlier version of
/// this function did, silently agreeing with itself that a value clamped to
/// exactly the ceiling was never cut at all.
template <typename T>
[[nodiscard]] inline std::pair<const void*, std::size_t> rawBytesOf(const T& value) noexcept
{
    if constexpr (ByteView<T>) {
        const Decayed<T> v(value);
        return {v.data(), v.size()};
    } else if constexpr (CStringLike<T>) {
        const char* const ptr = value;
        if (ptr == nullptr) {
            return {ptr, 0u};
        }
        std::size_t len = 0;
        while (len <= cArgCeiling && ptr[len] != '\0') {
            ++len;
        }
        return {ptr, len};
    } else if constexpr (StringViewLike<T>) {
        const std::string_view sv{value};
        return {sv.data(), sv.size()};
    } else {
        return {nullptr, 0u};
    }
}

/// Upper-bound contribution of one argument: the fixed wire size for a fixed
/// type, or 2 (u16 length) plus its bytes capped at cInlineBytesCap for a
/// view/string.
template <typename T>
[[nodiscard]] constexpr std::uint32_t upperBoundOne(const T& value) noexcept
{
    if constexpr (ByteView<T>) {
        const std::size_t len = Decayed<T>(value).size();
        const std::uint32_t capped =
            len < wire::cInlineBytesCap ? static_cast<std::uint32_t>(len)
                                        : static_cast<std::uint32_t>(wire::cInlineBytesCap);
        return 2u + capped;
    } else if constexpr (CStringLike<T>) {
        return 2u + boundedLength(value);
    } else if constexpr (StringViewLike<T>) {
        // Formed once here rather than reused from a caller-held view: the
        // conversion is read from the same const argument encodeArgs will
        // read moments later, so the two passes agree without needing to
        // share state between them.
        const std::string_view sv{value};
        const std::uint32_t capped =
            sv.size() < wire::cInlineBytesCap ? static_cast<std::uint32_t>(sv.size())
                                              : static_cast<std::uint32_t>(wire::cInlineBytesCap);
        return 2u + capped;
    } else {
        return fixedWireSize<T>();
    }
}

/// The encoder's size and the decoder's size for the same argument must be
/// identical; this asserts it per type rather than trusting the two tables
/// to stay in step.
template <typename T>
concept WireSizeAgrees =
    !FixedEncodable<T> || (fixedWireSize<T>() == wire::fixedSizeOf(typeCodeFor<T>()));

template <Encodable... Args>
[[nodiscard]] constexpr std::uint32_t upperBoundSize(const Args&... args) noexcept
{
    return (0u + ... + upperBoundOne(args));
}

/// Writes one fixed-size argument at dst and returns its wire size. The
/// caller has already sized the destination via upperBoundSize, so no
/// bounds check is made here (there is nothing sound to do on overflow
/// short of corrupting the record either way).
template <typename T>
inline std::uint32_t encodeFixedOne(std::byte* dst, const T& value) noexcept
{
    using U = Decayed<T>;
    if constexpr (std::same_as<U, bool>) {
        wire::storeUnaligned(dst, static_cast<std::uint8_t>(value ? 1u : 0u));
        return 1u;
    } else if constexpr (std::is_enum_v<U>) {
        wire::storeUnaligned(dst, static_cast<std::underlying_type_t<U>>(value));
        return static_cast<std::uint32_t>(sizeof(std::underlying_type_t<U>));
    } else if constexpr (std::is_pointer_v<U>) {
        // Widened, not reinterpreted at width: a 32-bit uintptr_t
        // zero-extends into the eight bytes TypeCode::Pointer means, so the
        // same address decodes the same way in a reader of either width.
        wire::storeUnaligned(
            dst, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(static_cast<U>(value))));
        return 8u;
    } else {
        wire::storeUnaligned(dst, static_cast<U>(value));
        return static_cast<std::uint32_t>(sizeof(U));
    }
}

/// Writes a u16 length then up to cInlineBytesCap bytes at dst, further
/// clamped to whatever room capacityLeft actually has (a defensive floor;
/// the caller's sizing already accounts for the cap). Sets truncated when
/// either clamp cut real data.
[[nodiscard]] inline std::uint32_t encodeBytesOne(std::byte* dst, std::uint32_t capacityLeft,
                                                  const void* data, std::size_t len,
                                                  bool& truncated) noexcept
{
    if (capacityLeft < 2u) {
        truncated = truncated || (len > 0u);
        return 0u;
    }
    std::uint32_t cap = static_cast<std::uint32_t>(wire::cInlineBytesCap);
    const std::uint32_t maxCopy = capacityLeft - 2u;
    if (cap > maxCopy) {
        cap = maxCopy;
    }
    const std::uint32_t copyLen =
        len < cap ? static_cast<std::uint32_t>(len) : cap;
    if (len > copyLen) {
        truncated = true;
    }
    wire::storeUnaligned(dst, static_cast<std::uint16_t>(copyLen));
    if (copyLen > 0u && data != nullptr) {
        std::memcpy(dst + 2, data, copyLen);
    }
    return 2u + copyLen;
}

template <Encodable... Args>
[[nodiscard]] EncodeResult encodeArgs(std::byte* dst, std::uint32_t capacity,
                                      const Args&... args) noexcept
{
    EncodeResult result{};
    std::uint32_t offset = 0;

    // maybe_unused: with an empty pack the fold below never names the
    // lambda, and GCC 13 warns on exactly that.
    [[maybe_unused]] auto encodeOne = [&]<typename T>(const T& value) {
        const std::uint32_t left = offset <= capacity ? capacity - offset : 0u;
        if constexpr (std::same_as<Decayed<T>, std::string_view>) {
            const std::string_view sv{value};
            offset += encodeBytesOne(dst + offset, left, sv.data(), sv.size(), result.truncated_);
        } else if constexpr (std::same_as<Decayed<T>, std::span<const std::byte>>) {
            const std::span<const std::byte> sp{value};
            offset += encodeBytesOne(dst + offset, left, sp.data(), sp.size(), result.truncated_);
        } else if constexpr (CStringLike<T>) {
            const char* const ptr = value;
            offset += encodeBytesOne(dst + offset, left, ptr, boundedLength(ptr), result.truncated_);
        } else if constexpr (StringViewLike<T>) {
            // Same conversion, formed fresh from the same const argument
            // upperBoundOne read -- see the comment there for why forming it
            // twice (once per pass) rather than caching it across the two
            // calls is what keeps them from disagreeing.
            const std::string_view sv{value};
            offset += encodeBytesOne(dst + offset, left, sv.data(), sv.size(), result.truncated_);
        } else {
            offset += encodeFixedOne(dst + offset, value);
        }
    };

    (encodeOne(args), ...);

    result.bytes_ = offset;
    return result;
}

} // namespace sub0log::detail
