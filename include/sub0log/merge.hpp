#pragma once

/** @file merge.hpp
 *  @brief Read-time merge of per-process segments into one ordered stream
 *         (R5.2). Ordering is a decoder concern by design
 *         (docs/multi-process.md).
 */

#include "reader.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
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
        std::uint64_t unreadableBytes_{}; ///< Damage: see SegmentReader.
        std::uint64_t unwrittenBytes_{};  ///< Never-written space; expected.
        std::uint64_t undecodableRecords_{};
    };

    /// The image must outlive the Merger (views point into it). Returns
    /// SegmentError::Ok, or why the segment could not be opened at all.
    [[nodiscard]] SegmentError addSegment(std::span<const std::byte> image);

    /// The returned records borrow twice: site_ points into this Merger's
    /// decoders, and string arguments view the added images -- so both the
    /// Merger and every image must outlive the result.
    ///
    /// Moves each DecodedRecord out of this Merger's own per-segment storage
    /// rather than copying it (docs/memory.md, "The reader" -- this used to
    /// be one allocation and one deep copy of `args_` per record, doubling
    /// peak memory for no reason: `Loaded::records_` is never read again
    /// after this runs, only `decoder_` is, for subsystemName()). That is
    /// why this is not `const`: call it once, after every addSegment() and
    /// before nothing else that needs the records themselves. A second call
    /// is not undefined behaviour -- the moved-from records are still there,
    /// just each with an empty `args_` -- but it is not a useful one either,
    /// so do not rely on merged() being repeatable the way addSegment(),
    /// totals() and subsystemName() are.
    [[nodiscard]] std::vector<MergedRecord> merged();
    [[nodiscard]] Totals totals() const noexcept { return totals_; }

    /// The name declared for `subsystem`, searched across every added
    /// segment in addSegment() order, first declaration wins -- the same
    /// rule Decoder::subsystemName() applies within one segment, extended
    /// across the merge so a MergedRecord (which does not itself carry
    /// which segment it came from) can still be labelled. Empty when no
    /// added segment ever declared it.
    [[nodiscard]] std::string_view subsystemName(SubsystemId subsystem) const noexcept;

private:
    // Kept per segment: the Decoder stays alive for the string_views it
    // handed out into the caller's image.
    struct Loaded {
        Decoder decoder_{};
        std::vector<DecodedRecord> records_{};
        wire::SegmentHeader header_{};
    };
    /// std::deque, not std::vector, and this is load-bearing rather than a
    /// preference. Every DecodedRecord handed out by merged() carries a
    /// `site_` pointer into its own segment's Decoder, so a container that
    /// relocates its elements invalidates them. A vector only *happened* to
    /// be safe: it relocates by move when the element is nothrow move
    /// constructible and by copy otherwise, and a Decoder was the former
    /// until it gained a std::deque member -- whose move constructor
    /// libstdc++ does not mark noexcept. That one addition silently flipped
    /// this vector from moving (which transfers hash nodes and keeps every
    /// pointer valid) to copying (which builds new nodes and then destroys
    /// the ones already pointed at). AddressSanitizer caught it as a
    /// use-after-free in the two-process merge test.
    ///
    /// A deque never relocates an element it already holds, so the
    /// invariant no longer depends on a property of Decoder that an
    /// unrelated change can take away.
    std::deque<Loaded> segments_{};
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

    // Both counters reflect the visit pass decodeAll just ran.
    totals_.unreadableBytes_ += reader.unreadableBytes();
    totals_.unwrittenBytes_ += reader.unwrittenBytes();
    totals_.undecodableRecords_ += loaded.decoder_.undecodableRecords();

    segments_.push_back(std::move(loaded));
    return SegmentError::Ok;
}

inline std::vector<MergedRecord> Merger::merged()
{
    std::size_t total = 0;
    for (const auto& seg : segments_) {
        total += seg.records_.size();
    }

    std::vector<MergedRecord> out;
    out.reserve(total);

    // Non-const seg, and rec taken by mutable reference below: this moves
    // each DecodedRecord's args_ vector into `out` instead of allocating a
    // second copy of it, since nothing looks at Loaded::records_ again once
    // this loop is done (see the class comment on why merged() is not const).
    for (auto& seg : segments_) {
        const std::uint64_t anchorMono = seg.header_.anchorMonoNs_;
        const std::uint64_t anchorWall = seg.header_.anchorWallNs_;
        const std::uint64_t processId = seg.header_.processId_;

        for (DecodedRecord& rec : seg.records_) {
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
            out.push_back(MergedRecord{std::move(rec), processId, alignedNs});
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

inline std::string_view Merger::subsystemName(const SubsystemId subsystem) const noexcept
{
    for (const Loaded& seg : segments_) {
        const std::string_view name = seg.decoder_.subsystemName(subsystem);
        if (!name.empty()) {
            return name;
        }
    }
    return {};
}

} // namespace sub0log
