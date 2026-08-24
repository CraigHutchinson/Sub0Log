#pragma once

/** @file reader.hpp
 *  @brief The consumer side: raw record recovery (SegmentReader) and typed
 *         decoding (Decoder). Reads files; never talks to a producer.
 *
 *  Recovery policy is tolerate-corrupted-tail per chunk
 *  (docs/framing-and-recovery.md): a zero or untagged head word ends that
 *  chunk's decode, the remainder is counted, and reading resumes at the next
 *  chunk boundary. The unreadable count is part of the result, not a side
 *  print (R3.3, R9.2).
 */

#include "severity.hpp"
#include "wire.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sub0log {

// ---------------------------------------------------------------------------
// Raw layer

/// A committed record as found in a chunk. Payload bytes are a view into the
/// caller's buffer; nothing is copied at this layer.
struct RecordView {
    wire::RecordHead head_{};
    std::span<const std::byte> payload_{};
    std::uint32_t chunkIndex_{};
    std::uint64_t ownerThread_{};
};

/// Why a segment could not be opened at all (damage *within* a valid segment
/// is not an error; it is counted).
enum class SegmentError : std::uint8_t {
    Ok = 0,
    TooSmall,
    BadMagic,
    UnknownVersion,   ///< Version gates everything after it (Kafka lesson).
    BadGeometry,      ///< Header geometry inconsistent with the file size.
};

/** Walks a segment image chunk by chunk, yielding committed records in
 *  chunk order (per-thread order; cross-thread order is the Merger's job).
 *
 *  Contract (TODO(impl:reader) — implement exactly this):
 *   - validate the SegmentHeader before touching any chunk; geometry from
 *     the header, never from defaults;
 *   - skip, and count in full, any chunk whose generation_ differs from the
 *     segment's (R3.4: positive evidence, not zero-fill inference);
 *   - per record: acquire-load the head word; stop the chunk on a missing
 *     commit tag; bounds-check payloadBytes against the chunk's remainder
 *     before touching the payload -- no length may steer the reader outside
 *     the chunk it was found in;
 *   - accumulate every byte not yielded as a record into unreadableBytes_.
 */
class SegmentReader {
public:
    /// On failure the result has valid() false and error() saying why.
    [[nodiscard]] static SegmentReader open(std::span<const std::byte> image) noexcept;

    [[nodiscard]] bool valid() const noexcept { return error_ == SegmentError::Ok; }
    [[nodiscard]] SegmentError error() const noexcept { return error_; }

    [[nodiscard]] const wire::SegmentHeader& header() const noexcept { return header_; }

    /// Visits every committed record. Returns the running unreadable-byte
    /// count (also available afterwards via unreadableBytes()).
    std::uint64_t
    visit(const std::function<void(const RecordView&)>& onRecord);

    [[nodiscard]] std::uint64_t unreadableBytes() const noexcept
    {
        return unreadableBytes_;
    }

private:
    SegmentReader() noexcept = default;

    std::span<const std::byte> image_{};
    wire::SegmentHeader header_{};
    SegmentError error_{SegmentError::TooSmall}; ///< Ok once open() validates.
    std::uint64_t unreadableBytes_{0};
};

// ---------------------------------------------------------------------------
// Typed layer

/// An argument as the producer typed it (R2.1): an integer arrives as an
/// integer. Bytes/string views point into the segment image.
using DecodedArg = std::variant<bool, char, std::int64_t, std::uint64_t,
                                double, std::string_view>;

/// The constant half, reassembled from a SiteDefinition record.
struct DecodedSite {
    std::uint64_t siteId_{};
    SubsystemId subsystem_{};
    Severity severity_{};
    std::uint32_t line_{};
    std::string_view format_{};
    std::string_view file_{};
    std::vector<wire::TypeCode> argTypes_{};
};

/// One message, typed. Filterable on every field without touching text
/// (R2.2); a test asserts on fields, not substrings (R2.3).
struct DecodedRecord {
    const DecodedSite* site_{nullptr};
    std::uint64_t monoNs_{};
    std::uint64_t correlationId_{};
    std::uint64_t ownerThread_{};
    bool truncated_{};
    std::vector<DecodedArg> args_{};
};

/** Builds the site table from definitions as they arrive (they precede first
 *  use by construction) and turns Message records into DecodedRecords.
 *
 *  Site ids are scoped to one segment: one Decoder per segment.
 *
 *  Formatting is the one place text is made, and only on request: format()
 *  renders via std::format-style substitution of "{}" placeholders. A record
 *  whose definition is missing (pre-truncation damage) is surfaced via
 *  onUndecodable, never dropped silently (R9.2).
 *
 *  TODO(impl:reader): implement decode(), format(), and the definition
 *  parser, bounds-checking every embedded length against the payload.
 */
class Decoder {
public:
    /// Consumes every RecordView from a SegmentReader::visit pass, in order:
    /// definitions update the table, Message records become DecodedRecords.
    /// Damage never fails the call -- it is counted (unreadable bytes on the
    /// reader, undecodable records here).
    [[nodiscard]] std::vector<DecodedRecord> decodeAll(SegmentReader& reader);

    [[nodiscard]] const DecodedSite* siteFor(std::uint64_t siteId) const noexcept;

    /// Records whose site definition never appeared (damage upstream).
    [[nodiscard]] std::uint64_t undecodableRecords() const noexcept
    {
        return undecodable_;
    }

    /// Renders one record's text: the site's format string with each "{}"
    /// replaced by the corresponding argument.
    [[nodiscard]] static std::string format(const DecodedRecord& record);

private:
    std::unordered_map<std::uint64_t, DecodedSite> sites_{};
    std::uint64_t undecodable_{0};
};

} // namespace sub0log
