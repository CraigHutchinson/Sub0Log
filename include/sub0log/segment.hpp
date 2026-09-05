#pragma once

/** @file segment.hpp
 *  @brief One process's segment: a mapped file of chunks, and the single
 *         atomic chunk claim.
 */

#include "chunk.hpp"
#include "detail/platform.hpp"
#include "wire.hpp"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <span>
#include <string>

namespace sub0log {

/// Segment geometry, in `sub0log` rather than `sub0log::detail` for the
/// same reason PlatformError is: a consumer fills it in. It reaches them as
/// `Logger::Options::segment_`, and sizing a segment -- which every example
/// with a deliberately small one does -- means writing
/// `options.segment_.segmentBytes_`. A configuration struct nobody can
/// configure without naming `detail::` is not a detail.
struct SegmentOptions {
    std::uint64_t segmentBytes_{wire::cDefaultSegmentBytes};
    std::uint32_t chunkBytes_{wire::cDefaultChunkBytes};
};

} // namespace sub0log

namespace sub0log::detail {

/// The spelling the segment layer itself has always used.
using SegmentOptions = ::sub0log::SegmentOptions;

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
    [[nodiscard]] PlatformError error() const noexcept
    {
        return geometryError_ ? geometryError_ : mapping_.error();
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Claims the next free chunk, stamps its header, returns a writer over
    /// its body. Invalid writer when the segment is full.
    [[nodiscard]] ChunkWriter claimChunk() noexcept;

private:
    FileMapping mapping_{};
    /// Set when create() refused the requested geometry; reported by error()
    /// ahead of the mapping's own, because the mapping was never attempted.
    PlatformError geometryError_{};
    std::string path_{};
    std::uint64_t generation_{0};
    std::uint32_t chunkBytes_{0};
    std::uint32_t chunkCount_{0};
    /// wire::sizeClassForChunkBytes(chunkBytes_), or 0 ("unspecified") when
    /// chunkBytes_ is not exactly representable as wire::cChunkSizeUnit << k
    /// -- purely additive (wire.hpp's cChunkSizeUnit comment), so an
    /// unrepresentable size still works exactly as it always did, just
    /// without the per-chunk self-description a representable one gets.
    std::uint8_t sizeClass_{0};
};

// ---------------------------------------------------------------------------
// Implementation

[[nodiscard]] inline Segment Segment::create(const std::string& directory,
                                             const std::string& stem,
                                             const SegmentOptions& options) noexcept
{
    Segment result{};

    const std::uint64_t generation = randomGeneration();
    const std::uint64_t pid = currentProcessId();

    // File name `<stem>-<pid>-<generation-hex>.s0l` (docs/architecture.md).
    char hexBuf[17]{};
    const auto convResult = std::to_chars(hexBuf, hexBuf + sizeof(hexBuf), generation, 16);
    const std::string hexStr(hexBuf, convResult.ptr);

    std::string path = directory;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += stem;
    path += '-';
    path += std::to_string(pid);
    path += '-';
    path += hexStr;
    path += ".s0l";

    const std::uint32_t headerBytes = wire::cSegmentHeaderBytes;
    const std::uint32_t chunkBytes = options.chunkBytes_;
    const std::uint64_t segmentBytes = options.segmentBytes_;

    // Geometry comes from a caller (Logger::Options::segment_ is public), so
    // it is validated before anything derives a size from it. Two ways it
    // can be poisonous, both caught here rather than in the reader:
    //   - chunkBytes <= sizeof(ChunkHeader) makes the body size underflow
    //     and hands ChunkWriter a span over most of the address space;
    //   - a chunk size that is not 8-aligned puts head words at 4-aligned
    //     addresses, where the atomic_ref the commit protocol depends on is
    //     undefined (and on strict-alignment targets, a fault).
    // The reader already refuses such a segment; a producer that can create
    // one only to have every reader reject it is worse than failing now.
    constexpr std::uint32_t cMinChunkBytes =
        static_cast<std::uint32_t>(sizeof(wire::ChunkHeader)) + 2u * wire::cRecordAlign;
    static_assert(wire::cChunkSizeUnit >= cMinChunkBytes,
                  "the smallest representable size class must itself be a valid chunk size");
    if (chunkBytes < cMinChunkBytes || (chunkBytes % wire::cRecordAlign) != 0u
        || (headerBytes % wire::cRecordAlign) != 0u) {
        result.mapping_ = FileMapping{};
        result.geometryError_ =
            PlatformError{0, "Segment::create: chunkBytes must be 8-aligned and larger "
                             "than a chunk header"};
        return result;
    }
    // Best-effort, never a rejection: a chunkBytes_ that happens not to be
    // wire::cChunkSizeUnit << k for any k just gets the 0 ("unspecified")
    // sentinel below, exactly as every chunk size did before this field
    // existed (wire.hpp's cChunkSizeUnit comment) -- every value this
    // codebase actually configures anywhere already is representable, but
    // nothing here requires a caller's choice to be.
    const std::uint8_t sizeClass = wire::sizeClassForChunkBytes(chunkBytes).value_or(0u);
    const std::uint32_t chunkCount =
        (segmentBytes > headerBytes && chunkBytes > 0u)
            ? static_cast<std::uint32_t>((segmentBytes - headerBytes) / chunkBytes)
            : 0u;

    FileMapping mapping = FileMapping::create(path, segmentBytes);
    result.path_ = path;
    if (!mapping.valid()) {
        result.mapping_ = std::move(mapping);
        return result;
    }

    // The anchor pair is read once, before any record can be written, so a
    // merger can align this segment's monotonic readings with wall-clock
    // time without decoding anything (R5.3).
    const std::uint64_t anchorMono = monotonicNowNs();
    const std::uint64_t anchorWall = wallNowNs();

    wire::SegmentHeader header{};
    header.magic_ = wire::cMagic;
    header.formatVersion_ = wire::cFormatVersion;
    header.reserved0_ = 0u;
    header.headerBytes_ = headerBytes;
    header.chunkBytes_ = chunkBytes;
    header.reserved1_ = 0u;
    header.segmentBytes_ = segmentBytes;
    header.generation_ = generation;
    header.processId_ = pid;
    header.anchorMonoNs_ = anchorMono;
    header.anchorWallNs_ = anchorWall;

    std::byte* const base = mapping.bytes().data();
    wire::storeUnaligned(base, header);

    // ftruncate already zero-fills a freshly created file, so the cursor is
    // zero regardless; stamped explicitly so the invariant does not rely on
    // that being remembered.
    wire::storeUnaligned(base + wire::cNextChunkOffset, std::uint64_t{0});

    result.mapping_ = std::move(mapping);
    result.generation_ = generation;
    result.chunkBytes_ = chunkBytes;
    result.chunkCount_ = chunkCount;
    result.sizeClass_ = sizeClass;
    return result;
}

[[nodiscard]] inline ChunkWriter Segment::claimChunk() noexcept
{
    if (!valid()) {
        return ChunkWriter{};
    }

    std::byte* const base = mapping_.bytes().data();
    // The only cross-thread synchronisation on the producer path (R1.3):
    // one fetch_add(relaxed) on the in-mapping cursor. startUint64LifetimeAt
    // (wire.hpp) is what makes forming this reference well-defined rather
    // than merely working.
    std::atomic_ref<std::uint64_t> cursor{
        *wire::startUint64LifetimeAt(base + wire::cNextChunkOffset)};
    const std::uint64_t index = cursor.fetch_add(1u, std::memory_order_relaxed);
    if (index >= chunkCount_) {
        return ChunkWriter{}; // exhausted; caller counts a drop (R9.1).
    }

    std::byte* const chunkBase =
        base + wire::cSegmentHeaderBytes + static_cast<std::uint64_t>(index) * chunkBytes_;

    // sizeClass_ was validated once, at create() (0 -- "unspecified" -- when
    // chunkBytes_ was not exactly representable): stamped verbatim here
    // rather than recomputed, since claimChunk() is the path R1.3 asks to
    // stay off any per-record cost, let alone a per-chunk lookup loop.
    const wire::ChunkHeader chunkHeader{
        generation_,
        currentThreadId(),
        monotonicNowNs(),
        sizeClass_,
    };
    wire::storeUnaligned(chunkBase, chunkHeader);

    std::span<std::byte> body{chunkBase + sizeof(wire::ChunkHeader),
                              chunkBytes_ - sizeof(wire::ChunkHeader)};
    return ChunkWriter{body};
}

} // namespace sub0log::detail
