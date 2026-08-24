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

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

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

template <typename T>
concept FixedEncodable = std::same_as<Decayed<T>, bool>
                      || std::same_as<Decayed<T>, char>
                      || (std::integral<Decayed<T>> && sizeof(Decayed<T>) <= 8)
                      || std::floating_point<Decayed<T>>
                      || std::is_enum_v<Decayed<T>>
                      || std::is_pointer_v<Decayed<T>>;

/// The full set a call site may pass. Everything else is rejected at compile
/// time by typeCodeFor's static_assert, which names the two escape hatches.
template <typename T>
concept Encodable = FixedEncodable<T> || ByteView<T> || CStringLike<T>;

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
    else if constexpr (ByteView<T> || CStringLike<T>) { return wire::TypeCode::Bytes; }
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
        static_assert(FixedEncodable<T> || ByteView<T> || CStringLike<T>,
                      "Sub0Log arguments must be trivially copyable values or "
                      "byte views. For text pass std::string_view (bytes are "
                      "inlined, capped); for large or owned data log an "
                      "identifier instead. A std::string will not be silently "
                      "copied -- that hidden allocation is what this refusal "
                      "prevents (docs/record-model.md).");
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
/// of its underlying type, a pointer is sizeof(uintptr_t), everything else
/// is its own size (already true by construction of FixedEncodable).
template <typename T>
[[nodiscard]] constexpr std::uint32_t fixedWireSize() noexcept
{
    using U = Decayed<T>;
    if constexpr (std::same_as<U, bool>) {
        return 1u;
    } else if constexpr (std::is_enum_v<U>) {
        return static_cast<std::uint32_t>(sizeof(std::underlying_type_t<U>));
    } else if constexpr (std::is_pointer_v<U>) {
        return static_cast<std::uint32_t>(sizeof(std::uintptr_t));
    } else {
        return static_cast<std::uint32_t>(sizeof(U));
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
        const char* const ptr = value;
        const std::size_t len = (ptr != nullptr) ? std::char_traits<char>::length(ptr) : std::size_t{0};
        const std::uint32_t capped =
            len < wire::cInlineBytesCap ? static_cast<std::uint32_t>(len)
                                        : static_cast<std::uint32_t>(wire::cInlineBytesCap);
        return 2u + capped;
    } else {
        return fixedWireSize<T>();
    }
}

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
        wire::storeUnaligned(dst, reinterpret_cast<std::uintptr_t>(static_cast<U>(value)));
        return static_cast<std::uint32_t>(sizeof(std::uintptr_t));
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

    auto encodeOne = [&]<typename T>(const T& value) {
        const std::uint32_t left = offset <= capacity ? capacity - offset : 0u;
        if constexpr (std::same_as<Decayed<T>, std::string_view>) {
            const std::string_view sv{value};
            offset += encodeBytesOne(dst + offset, left, sv.data(), sv.size(), result.truncated_);
        } else if constexpr (std::same_as<Decayed<T>, std::span<const std::byte>>) {
            const std::span<const std::byte> sp{value};
            offset += encodeBytesOne(dst + offset, left, sp.data(), sp.size(), result.truncated_);
        } else if constexpr (CStringLike<T>) {
            const char* const ptr = value;
            const std::size_t len = (ptr != nullptr) ? std::char_traits<char>::length(ptr) : std::size_t{0};
            offset += encodeBytesOne(dst + offset, left, ptr, len, result.truncated_);
        } else {
            offset += encodeFixedOne(dst + offset, value);
        }
    };

    (encodeOne(args), ...);

    result.bytes_ = offset;
    return result;
}

} // namespace sub0log::detail
