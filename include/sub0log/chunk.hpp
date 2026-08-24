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
 *  Sequence ownership: the caller passes commit() a head with sequence_
 *  unset (0); commit() is the sole writer of sequence_ and is what
 *  increments the per-chunk counter. This works because a ChunkWriter is
 *  used by exactly one thread and reserve()/commit() are always paired
 *  before the next reserve() -- there is never a second reservation in
 *  flight whose commit could land out of order, so "reserve order" and
 *  "commit order" are the same order and either could have been the owner;
 *  commit was chosen because it is the point closest to the write actually
 *  becoming visible to a reader.
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
        [[nodiscard]] bool valid() const noexcept { return headWord_ != nullptr; }
    };

    [[nodiscard]] Reservation reserve(std::uint32_t payloadBytes) noexcept;

    /// Release-store of the packed head word; the payload must be fully
    /// written first. Increments the per-chunk sequence.
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
    return Reservation{payload, headWord};
}

inline void ChunkWriter::commit(const Reservation& slot, wire::RecordHead head) noexcept
{
    head.sequence_ = sequence_;
    ++sequence_;
    // The head-word slot is 8-byte aligned by construction (reserve() only
    // hands out slots at 8-aligned offsets within an 8-aligned chunk body).
    // atomic_ref over that uint64_t-sized, uint64_t-aligned storage is the
    // producer's one piece of cross-thread synchronisation with the reader
    // (R1.3): release here, acquire on the read side.
    std::atomic_ref<std::uint64_t> headRef{
        *reinterpret_cast<std::uint64_t*>(slot.headWord_)};
    headRef.store(head.pack(), std::memory_order_release);
}

} // namespace sub0log::detail
