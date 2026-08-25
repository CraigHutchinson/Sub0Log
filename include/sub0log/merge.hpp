#pragma once

/** @file merge.hpp
 *  @brief Read-time merge of per-process segments into one ordered stream
 *         (R5.2). Ordering is a decoder concern by design
 *         (docs/multi-process.md).
 */

#include "reader.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
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
 *  addSegment() decodes one segment image, keeping its Decoder alive for the
 *  string_views it hands out; merged() returns all records ordered by
 *  alignedNs_, ties broken by (processId, ownerThread, monoNs) for
 *  determinism. Unreadable-byte and undecodable-record counts are summed
 *  across segments and reported, never swallowed (R3.3, R9.2).
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

    /// The returned records borrow twice: site_ points into this Merger's
    /// decoders, and string arguments view the added images -- so both the
    /// Merger and every image must outlive the result.
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

inline SegmentError Merger::addSegment(std::span<const std::byte> image)
{
    SegmentReader reader = SegmentReader::open(image);
    if (!reader.valid()) {
        return reader.error();
    }

    Loaded loaded;
    loaded.header_ = reader.header();
    loaded.records_ = loaded.decoder_.decodeAll(reader); // runs the visit pass

    // unreadableBytes() reflects the visit pass decodeAll just ran.
    totals_.unreadableBytes_ += reader.unreadableBytes();
    totals_.undecodableRecords_ += loaded.decoder_.undecodableRecords();

    segments_.push_back(std::move(loaded));
    return SegmentError::Ok;
}

inline std::vector<MergedRecord> Merger::merged() const
{
    std::size_t total = 0;
    for (const auto& seg : segments_) {
        total += seg.records_.size();
    }

    std::vector<MergedRecord> out;
    out.reserve(total);

    for (const auto& seg : segments_) {
        const std::uint64_t anchorMono = seg.header_.anchorMonoNs_;
        const std::uint64_t anchorWall = seg.header_.anchorWallNs_;
        const std::uint64_t processId = seg.header_.processId_;

        for (const DecodedRecord& rec : seg.records_) {
            std::uint64_t alignedNs;
            if (rec.monoNs_ >= anchorMono) {
                alignedNs = anchorWall + (rec.monoNs_ - anchorMono);
            } else {
                // rec.monoNs_ < anchorMono is only possible if the
                // machine-wide monotonic source went backwards between the
                // anchor reading and this record's -- clocks misbehaving,
                // not normal operation. (rec.monoNs_ - anchorMono) would
                // underflow a std::uint64_t and wrap to a huge value,
                // sorting the misbehaving record as the newest thing in the
                // merged stream; saturating at the anchor instead keeps it
                // pinned near where it was actually written.
                alignedNs = anchorWall;
            }
            out.push_back(MergedRecord{rec, processId, alignedNs});
        }
    }

    // Ties broken by (processId, ownerThread, monoNs) for determinism
    // (R5.2/R5.3): two records that align to the same instant still need a
    // reproducible order across runs of the same input.
    std::stable_sort(out.begin(), out.end(), [](const MergedRecord& a, const MergedRecord& b) {
        if (a.alignedNs_ != b.alignedNs_) {
            return a.alignedNs_ < b.alignedNs_;
        }
        if (a.processId_ != b.processId_) {
            return a.processId_ < b.processId_;
        }
        if (a.record_.ownerThread_ != b.record_.ownerThread_) {
            return a.record_.ownerThread_ < b.record_.ownerThread_;
        }
        return a.record_.monoNs_ < b.record_.monoNs_;
    });

    return out;
}

} // namespace sub0log
