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
 *   1. reserve(paddedPayload(n) + 8): fits the chunk's remainder or fails --
 *      never a partial record spanning chunks;
 *   2. payload is memcpy'd after the head-word slot;
 *   3. commit(head): release-store of head.pack() into the slot. The record
 *      exists, in kernel-owned memory, when this store completes (R3.1).
 *
 *  TODO(impl:producer): implement reserve/commit per the above; keep the
 *  sequence counter per chunk; expose remaining() for the emit path's
 *  fit-or-truncate decision.
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

} // namespace sub0log::detail
