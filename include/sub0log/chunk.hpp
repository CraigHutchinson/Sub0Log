#pragma once

/** @file chunk.hpp
 *  @brief One thread's bump-allocating writer over one claimed chunk.
 */

#include "wire.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sub0log::detail {

// R1.3 promises the producer takes no lock. The commit store and the chunk
// claim are both 64-bit atomics, and a 64-bit atomic is only lock-free where
// the core has a 64-bit read-modify-write: on a 32-bit target without one
// (Cortex-M, older ARM), the standard library silently substitutes a lock
// table, so the guarantee would quietly become false rather than fail. This
// makes that a build error naming the reason instead.
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "Sub0Log needs lock-free 64-bit atomics: the commit head word "
              "and the chunk-claim cursor are both u64, and R1.3 promises no "
              "lock on the producer path. On a target without a 64-bit RMW "
              "the standard library would fall back to a lock table and the "
              "promise would silently stop holding.");

/** Owned by exactly one thread after the claim, so nothing here synchronises
 *  except the commit store (R1.3).
 *
 *  Write protocol per record (docs/framing-and-recovery.md, "commit last"):
 *   1. reserve(n): fails if paddedPayload(n) + 8 does not fit the chunk's
 *      remainder -- never a partial record spanning chunks;
 *   2. payload is memcpy'd (by the caller) after the head-word slot;
 *   3. commit(head): release-store of head.pack() into the slot. The record
 *      exists, in kernel-owned memory, when this store completes (R3.1).
 *
 *  Sequence ownership: reserve() is the sole writer of the per-chunk
 *  sequence counter, and stamps the value it hands out into the
 *  Reservation; commit() only copies that value into the head word it
 *  stores. Sequence order must track *offset* order, not *commit* order --
 *  the record's position in the chunk is what a reader would trust it for,
 *  the way a torn-write check already trusts payloadBytes_ against the
 *  chunk it was found in -- and a continuation chain is exactly the case
 *  where those two orders diverge: the Message reserves before its
 *  Continuations (it is written first) but must commit after them (a
 *  reader must never observe a chain with its head published and its tail
 *  still missing, so the tail commits first). Assigning at commit time, as
 *  this class used to, would have given the Message a *higher* sequence
 *  than the continuations physically ahead of it in the chunk -- nothing
 *  reads that field today, but the property "sequence increases with
 *  offset" would have silently stopped holding for exactly the one record
 *  kind that needs several outstanding reservations at once (detail/emit.hpp).
 */
class ChunkWriter {
public:
    ChunkWriter() noexcept = default;

    /// Binds to a freshly claimed chunk whose ChunkHeader the segment has
    /// already stamped. `body` excludes the ChunkHeader.
    explicit ChunkWriter(std::span<std::byte> body) noexcept;

    [[nodiscard]] bool valid() const noexcept { return !body_.empty(); }
    [[nodiscard]] std::uint32_t remaining() const noexcept;

    /// A slot handed out by reserve(): payload destination plus the head-word
    /// location the commit will fill.
    struct Reservation {
        std::byte* payload_{nullptr};
        std::byte* headWord_{nullptr};
        /// Assigned by reserve(), in offset order; commit() copies it
        /// verbatim rather than assigning its own (see the class comment,
        /// "Sequence ownership").
        std::uint16_t sequence_{0};
        [[nodiscard]] bool valid() const noexcept { return headWord_ != nullptr; }
    };

    [[nodiscard]] Reservation reserve(std::uint32_t payloadBytes) noexcept;

    /// Release-store of the packed head word; the payload must be fully
    /// written first. Stamps head.sequence_ from the Reservation (assigned
    /// back in reserve(), not here -- see the class comment).
    void commit(const Reservation& slot, wire::RecordHead head) noexcept;

private:
    std::span<std::byte> body_{};
    std::uint32_t offset_{0};
    std::uint16_t sequence_{0};
};

// ---------------------------------------------------------------------------
// Implementation

inline ChunkWriter::ChunkWriter(std::span<std::byte> body) noexcept
    : body_{body}, offset_{0}, sequence_{0}
{
}

[[nodiscard]] inline std::uint32_t ChunkWriter::remaining() const noexcept
{
    if (!valid() || offset_ > body_.size()) {
        return 0u;
    }
    return static_cast<std::uint32_t>(body_.size()) - offset_;
}

[[nodiscard]] inline ChunkWriter::Reservation ChunkWriter::reserve(std::uint32_t payloadBytes) noexcept
{
    // The head word carries the payload length as u16; a payload that would
    // wrap it cannot be represented, whatever the chunk size says. Refusing
    // here covers every caller (chunks above 64 KiB are configurable).
    if (payloadBytes > 0xFFFFu) {
        return Reservation{};
    }
    const std::uint32_t padded = wire::paddedPayload(payloadBytes);
    const std::uint32_t total = 8u + padded;
    if (!valid() || total > remaining()) {
        return Reservation{};
    }
    std::byte* const headWord = body_.data() + offset_;
    std::byte* const payload = headWord + 8u;
    offset_ += total;
    const std::uint16_t sequence = sequence_;
    ++sequence_;
    return Reservation{payload, headWord, sequence};
}

inline void ChunkWriter::commit(const Reservation& slot, wire::RecordHead head) noexcept
{
    head.sequence_ = slot.sequence_;
    // The head-word slot is 8-byte aligned by construction (reserve() only
    // hands out slots at 8-aligned offsets within an 8-aligned chunk body).
    // atomic_ref over that uint64_t-sized, uint64_t-aligned storage is the
    // producer's one piece of cross-thread synchronisation with the reader
    // (R1.3): release here, acquire on the read side. startUint64LifetimeAt
    // (wire.hpp) is what makes forming this reference well-defined rather
    // than merely working.
    std::atomic_ref<std::uint64_t> headRef{
        *wire::startUint64LifetimeAt(slot.headWord_)};
    headRef.store(head.pack(), std::memory_order_release);
}

} // namespace sub0log::detail
