#pragma once

/** @file severity.hpp
 *  @brief The library-owned severity ladder, and the consumer-owned
 *         subsystem id.
 *
 *  The ladder lives in the library because drop and retention rules key off
 *  it; the subsystem vocabulary stays with the consumer (docs/record-model.md,
 *  "The library must not own the vocabulary").
 */

#include <cstdint>

namespace sub0log {

/// Wire-stable values: these bytes appear in SiteDefinition records, so a
/// renumbering is a format version bump, not a refactor.
enum class Severity : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    /// A failure the library (or the caller) could not classify. Deliberately
    /// above Error so no threshold that admits errors can hide it (R9.2).
    Unclassified = 5,
    Fatal = 6,
};

/// True when `value` is at or above `threshold` on the ladder above.
/// Parameter order matters and both are plain `Severity` values with
/// nothing at the call site to catch a swap: `atLeast(a, b)` and
/// `atLeast(b, a)` both compile and disagree whenever `a != b`. Read it as
/// "is `value` severe enough for `threshold`" -- the same order
/// `Logger::threshold()` and every filter in this library use.
[[nodiscard]] constexpr bool atLeast(const Severity value, const Severity threshold) noexcept
{
    return static_cast<std::uint8_t>(value) >= static_cast<std::uint8_t>(threshold);
}

/// Opaque, consumer-defined: the library never enumerates subsystems and has
/// no opinion about what a given id means. The consumer's name for one can
/// travel inside the segment itself -- Logger::Options::subsystemNames_ at
/// construction, or Logger::declareSubsystem() for one discovered later --
/// via a SubsystemDefinition record (docs/record-model.md, "The library must
/// not own the vocabulary"). Decoder::subsystemName() reads it back; an id
/// nobody declared decodes to an empty name, never a guess.
struct SubsystemId {
    std::uint32_t value_{};

    [[nodiscard]] constexpr bool operator==(const SubsystemId&) const noexcept = default;
};

/// Call-site macros accept either a SubsystemId constant or a bare integer;
/// this pair is the whole of that flexibility.
[[nodiscard]] constexpr SubsystemId toSubsystemId(const SubsystemId id) noexcept { return id; }
[[nodiscard]] constexpr SubsystemId toSubsystemId(const std::uint32_t value) noexcept
{
    return SubsystemId{value};
}

} // namespace sub0log
