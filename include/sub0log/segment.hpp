#pragma once

/** @file segment.hpp
 *  @brief One process's segment: a mapped file of chunks, and the single
 *         atomic chunk claim.
 */

#include "chunk.hpp"
#include "detail/platform.hpp"
#include "wire.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace sub0log::detail {

struct SegmentOptions {
    std::uint64_t segmentBytes_{wire::cDefaultSegmentBytes};
    std::uint32_t chunkBytes_{wire::cDefaultChunkBytes};
};

/** Creates the file at full size, maps it shared, writes the SegmentHeader
 *  (magic, version, geometry, random generation, process id, anchor pair)
 *  before any record can be written.
 *
 *  claimChunk() is the only cross-thread operation on the producer path:
 *  one fetch_add(relaxed) on the in-mapping cursor at wire::cNextChunkOffset,
 *  then the claiming thread stamps the ChunkHeader (generation, thread id,
 *  claim time) and owns the chunk outright.
 *
 *  Exhaustion returns an invalid writer; the caller counts a drop (R9.1).
 *  Nothing blocks, nothing allocates, nothing grows (R1.2, R1.3).
 *
 *  TODO(impl:producer): implement create() and claimChunk() per the above.
 */
class Segment {
public:
    Segment() noexcept = default;
    Segment(Segment&&) noexcept = default;
    Segment& operator=(Segment&&) noexcept = default;

    /// Path is `<directory>/<stem>-<pid>-<generation>.s0l`. On failure the
    /// result has valid() false and error() set.
    [[nodiscard]] static Segment create(const std::string& directory,
                                        const std::string& stem,
                                        const SegmentOptions& options = {}) noexcept;

    [[nodiscard]] bool valid() const noexcept { return mapping_.valid(); }
    [[nodiscard]] PlatformError error() const noexcept { return mapping_.error(); }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Claims the next free chunk, stamps its header, returns a writer over
    /// its body. Invalid writer when the segment is full.
    [[nodiscard]] ChunkWriter claimChunk() noexcept;

private:
    FileMapping mapping_{};
    std::string path_{};
    std::uint64_t generation_{0};
    std::uint32_t chunkBytes_{0};
    std::uint32_t chunkCount_{0};
};

} // namespace sub0log::detail
