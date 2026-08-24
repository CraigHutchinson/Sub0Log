#pragma once

/** @file merge.hpp
 *  @brief Read-time merge of per-process segments into one ordered stream
 *         (R5.2). Ordering is a decoder concern by design
 *         (docs/multi-process.md).
 */

#include "reader.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sub0log {

/// A record with its cross-process timeline position. alignedNs_ is
/// anchorWall + (mono - anchorMono) of the record's own segment -- the
/// anchor arithmetic that makes readings from different processes
/// comparable (R5.3).
struct MergedRecord {
    DecodedRecord record_{};
    std::uint64_t processId_{};
    std::uint64_t alignedNs_{};
};

/** N-way merge over decoded segments, ordered by alignedNs_.
 *
 *  TODO(impl:reader): addSegment() decodes one segment image (keeping its
 *  Decoder alive for the string_views it hands out); merged() returns all
 *  records ordered by alignedNs_, ties broken by (processId, ownerThread,
 *  monoNs) for determinism. Unreadable-byte and undecodable-record counts
 *  are summed across segments and reported, never swallowed (R3.3, R9.2).
 */
class Merger {
public:
    struct Totals {
        std::uint64_t unreadableBytes_{};
        std::uint64_t undecodableRecords_{};
    };

    /// The image must outlive the Merger (views point into it). Returns
    /// SegmentError::Ok, or why the segment could not be opened at all.
    [[nodiscard]] SegmentError addSegment(std::span<const std::byte> image);

    [[nodiscard]] std::vector<MergedRecord> merged() const;
    [[nodiscard]] Totals totals() const noexcept { return totals_; }

private:
    // Kept per segment: the Decoder stays alive for the string_views it
    // handed out into the caller's image.
    struct Loaded {
        Decoder decoder_{};
        std::vector<DecodedRecord> records_{};
        wire::SegmentHeader header_{};
    };
    std::vector<Loaded> segments_{};
    Totals totals_{};
};

} // namespace sub0log
