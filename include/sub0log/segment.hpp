#pragma once

/** @file segment.hpp
 *  @brief One process's segment: a mapped file (or caller-owned memory) of
 *         chunks, and the single atomic chunk claim.
 */

#include "chunk.hpp"
#include "detail/platform.hpp"
#include "wire.hpp"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

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
 *  Or, through createInMemory(), does the same over memory the caller owns
 *  (issue #2, docs/vnext-backends-and-memory.md rung 1). Everything past
 *  create is identical either way: claimChunk() only ever sees `bytes_`, so
 *  the backing is decided once, off the producer path, and a record written
 *  into caller memory is byte-for-byte what the file would have held --
 *  the same SegmentReader/Decoder/Merger read it. What changes is only what
 *  survives: see createInMemory().
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
    // Not defaulted: `bytes_` is a view, and a defaulted move would leave the
    // moved-from Segment still valid() over the same memory -- two claim
    // paths onto one cursor, one of them owned by nobody.
    Segment(Segment&& other) noexcept;
    Segment& operator=(Segment&& other) noexcept;

    /// Path is `<directory>/<stem>-<pid>-<generation>.s0l`. On failure the
    /// result has valid() false and error() set.
    [[nodiscard]] static Segment create(const std::string& directory,
                                        const std::string& stem,
                                        const SegmentOptions& options = {}) noexcept;

    /** A segment over `storage`, which the caller owns and must keep alive
     *  (and unmoved) for as long as this Segment -- and so the Logger holding
     *  it -- exists. No file, no mapping call, no allocation: the whole
     *  storage is zeroed, the header written, and that is all.
     *
     *  The segment is `storage.size()` bytes; `options.segmentBytes_` is not
     *  consulted. The header area is wire::cCompactSegmentHeaderBytes (192),
     *  not a file segment's 4096. `storage` must be aligned to wire::cRecordAlign (a
     *  `alignas(8) std::byte[N]` or any `new`/`malloc` result is), and large
     *  enough for the header and one chunk; otherwise the result is invalid
     *  with error() saying which.
     *
     *  What this gives up is R3, by declared choice rather than accident:
     *  the records live in process memory, so a hard kill takes them unless
     *  the caller's storage itself outlives the process (a retained-RAM
     *  region that survives a warm reset, a shared mapping a supervisor also
     *  holds). The library does not know which, and does not claim either.
     */
    [[nodiscard]] static Segment createInMemory(std::span<std::byte> storage,
                                                const SegmentOptions& options = {}) noexcept;

    [[nodiscard]] bool valid() const noexcept { return bytes_.data() != nullptr; }
    [[nodiscard]] PlatformError error() const noexcept
    {
        return geometryError_ ? geometryError_ : mapping_.error();
    }
    /// Empty for an in-memory segment: there is no file.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// What a call site records to remember it has been defined in *this*
    /// segment (SiteDescriptor::announcedKey_); never 0. The generation
    /// where 64-bit atomics are lock-free; on the split path a u32 from a
    /// per-process counter, unique for 2^32 - 1 segment creations
    /// (detail::AnnounceWord says why the two differ).
    [[nodiscard]] detail::AnnounceWord announceKey() const noexcept
    {
        if constexpr (detail::cSplitAtomics) {
            return announceKey_;
        } else {
            return generation_;
        }
    }

    /// Claims the next free chunk, stamps its header, returns a writer over
    /// its body. Invalid writer when the segment is full.
    [[nodiscard]] ChunkWriter claimChunk() noexcept;

    /// Chunks handed out so far, never more than chunkCount(). One relaxed
    /// load of the claim cursor: a snapshot for a control thread deciding
    /// when to rotate or drain, not a synchronisation point.
    [[nodiscard]] std::uint32_t claimedChunks() const noexcept;
    /// 0 when invalid -- including a moved-from Segment, whose other
    /// geometry fields are left as they were.
    [[nodiscard]] std::uint32_t chunkCount() const noexcept { return valid() ? chunkCount_ : 0u; }

private:
    /// Refuses a chunk size the wire format cannot carry; the empty error
    /// when it is fine. Shared by both creation paths.
    [[nodiscard]] static PlatformError validateChunkBytes(std::uint32_t chunkBytes) noexcept;

    /// Stamps the header and cursor over `bytes` and records the geometry --
    /// everything both creation paths do once the memory exists.
    void initialise(std::span<std::byte> bytes, std::uint32_t headerBytes,
                    std::uint32_t chunkBytes, std::uint64_t generation) noexcept;

    /// The segment's bytes, whoever owns them: mapping_'s view for a file
    /// segment, the caller's span for an in-memory one. Null when invalid.
    std::span<std::byte> bytes_{};
    /// Owns the file mapping; empty for an in-memory segment.
    FileMapping mapping_{};
    /// Set when create() refused the requested geometry; reported by error()
    /// ahead of the mapping's own, because the mapping was never attempted.
    PlatformError geometryError_{};
    std::string path_{};
    std::uint64_t generation_{0};
    /// The split path's announce key; unused where the generation is the key.
    std::uint32_t announceKey_{0};
    /// Where chunk 0 starts: wire::cSegmentHeaderBytes for a file,
    /// wire::cCompactSegmentHeaderBytes in memory. Written into the header,
    /// which is where a reader takes it from.
    std::uint32_t headerBytes_{0};
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

inline Segment::Segment(Segment&& other) noexcept
    : bytes_{std::exchange(other.bytes_, {})},
      mapping_{std::move(other.mapping_)},
      geometryError_{other.geometryError_},
      path_{std::move(other.path_)},
      generation_{other.generation_},
      announceKey_{other.announceKey_},
      headerBytes_{other.headerBytes_},
      chunkBytes_{other.chunkBytes_},
      chunkCount_{other.chunkCount_},
      sizeClass_{other.sizeClass_}
{
}

inline Segment& Segment::operator=(Segment&& other) noexcept
{
    if (this != &other) {
        bytes_ = std::exchange(other.bytes_, {});
        mapping_ = std::move(other.mapping_);
        geometryError_ = other.geometryError_;
        path_ = std::move(other.path_);
        generation_ = other.generation_;
        announceKey_ = other.announceKey_;
        headerBytes_ = other.headerBytes_;
        chunkBytes_ = other.chunkBytes_;
        chunkCount_ = other.chunkCount_;
        sizeClass_ = other.sizeClass_;
    }
    return *this;
}

[[nodiscard]] inline PlatformError Segment::validateChunkBytes(const std::uint32_t chunkBytes) noexcept
{
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
    static_assert(wire::cSegmentHeaderBytes % wire::cRecordAlign == 0u);
    static_assert(wire::cCompactSegmentHeaderBytes % wire::cRecordAlign == 0u);
    if (chunkBytes < cMinChunkBytes || (chunkBytes % wire::cRecordAlign) != 0u) {
        return PlatformError{0, "Segment::create: chunkBytes must be 8-aligned and larger "
                                "than a chunk header"};
    }
    return PlatformError{};
}

inline void Segment::initialise(const std::span<std::byte> bytes, const std::uint32_t headerBytes,
                                const std::uint32_t chunkBytes,
                                const std::uint64_t generation) noexcept
{
    const std::uint64_t segmentBytes = bytes.size();

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
    header.processId_ = currentProcessId();
    header.anchorMonoNs_ = anchorMono;
    header.anchorWallNs_ = anchorWall;

    std::byte* const base = bytes.data();
    wire::storeUnaligned(base, header);

    // A fresh file is already zero-filled (ftruncate) and caller memory has
    // been zeroed by createInMemory(), so the cursor is zero regardless;
    // stamped explicitly so the invariant does not rely on that being
    // remembered.
    wire::storeUnaligned(base + wire::cNextChunkOffset, std::uint64_t{0});

    bytes_ = bytes;
    generation_ = generation;
    headerBytes_ = headerBytes;
    if constexpr (detail::cSplitAtomics) {
        // Control-thread work, once per segment. 0 is what an unannounced
        // site holds, so a wrap skips it.
        //
        // Seeded from the first generation rather than starting at 1: this
        // counter exists once per shared object where the library is
        // instantiated more than once (hidden visibility), and a host can
        // force this path (SUB0LOG_SPLIT_ATOMICS). Random starts make two
        // objects' key ranges overlap with probability ~ n_a * n_b / 2^32
        // rather than certainly; within one image keys stay exact.
        static constinit std::atomic<std::uint32_t> sNextAnnounceKey{0};
        std::uint32_t unseeded = 0;
        (void)sNextAnnounceKey.compare_exchange_strong(
            unseeded, static_cast<std::uint32_t>(generation) | 1u, std::memory_order_relaxed);
        std::uint32_t key = sNextAnnounceKey.fetch_add(1u, std::memory_order_relaxed);
        if (key == 0u) {
            key = sNextAnnounceKey.fetch_add(1u, std::memory_order_relaxed);
        }
        announceKey_ = key;
    }
    chunkBytes_ = chunkBytes;
    chunkCount_ = segmentBytes > headerBytes
                      ? static_cast<std::uint32_t>((segmentBytes - headerBytes) / chunkBytes)
                      : 0u;
    // Best-effort, never a rejection: a chunkBytes_ that happens not to be
    // wire::cChunkSizeUnit << k for any k just gets the 0 ("unspecified")
    // sentinel, exactly as every chunk size did before this field existed
    // (wire.hpp's cChunkSizeUnit comment) -- every value this codebase
    // actually configures anywhere already is representable, but nothing
    // here requires a caller's choice to be.
    sizeClass_ = wire::sizeClassForChunkBytes(chunkBytes).value_or(0u);
}

[[nodiscard]] inline Segment Segment::createInMemory(const std::span<std::byte> storage,
                                                     const SegmentOptions& options) noexcept
{
    Segment result{};
    if (const PlatformError bad = validateChunkBytes(options.chunkBytes_)) {
        result.geometryError_ = bad;
        return result;
    }
    // The cursor at wire::cNextChunkOffset is an atomic_ref target, and every
    // head word after it is too: an under-aligned base makes the whole
    // commit protocol undefined, not merely slow.
    constexpr std::size_t cAlign =
        detail::AtomicRef<std::uint64_t>::required_alignment > wire::cRecordAlign
            ? detail::AtomicRef<std::uint64_t>::required_alignment
            : wire::cRecordAlign;
    if (storage.data() == nullptr
        || reinterpret_cast<std::uintptr_t>(storage.data()) % cAlign != 0u) {
        result.geometryError_ =
            PlatformError{0, "Segment::createInMemory: storage must be non-null and 8-aligned"};
        return result;
    }
    if (storage.size() < std::uint64_t{wire::cCompactSegmentHeaderBytes} + options.chunkBytes_) {
        result.geometryError_ = PlatformError{
            0, "Segment::createInMemory: storage is smaller than the header plus one chunk"};
        return result;
    }

    // Caller memory is not the freshly-truncated file the reader's "a zero
    // head word is unwritten" rule was written against: whatever it held
    // before would read as records. Zeroed once, here, on the control
    // thread -- claimChunk() stays exactly as cheap as the file path's.
    std::memset(storage.data(), 0, storage.size());
    result.initialise(storage, wire::cCompactSegmentHeaderBytes, options.chunkBytes_,
                      randomGeneration());
    return result;
}

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

    if (const PlatformError bad = validateChunkBytes(options.chunkBytes_)) {
        result.geometryError_ = bad;
        return result;
    }

    FileMapping mapping = FileMapping::create(path, options.segmentBytes_);
    result.path_ = path;
    if (!mapping.valid()) {
        result.mapping_ = std::move(mapping);
        return result;
    }

    const std::span<std::byte> bytes = mapping.bytes();
    result.mapping_ = std::move(mapping); // moves the handle; the view is unchanged
    // The generation already named the file; the header must carry the same.
    result.initialise(bytes, wire::cSegmentHeaderBytes, options.chunkBytes_, generation);
    return result;
}

[[nodiscard]] inline std::uint32_t Segment::claimedChunks() const noexcept
{
    if (!valid()) {
        return 0u;
    }
    // Clamped: on 64-bit-atomic cores the cursor keeps counting refused
    // claims past the end (detail::claimChunkIndex).
    return detail::loadClaimedChunks(bytes_.data() + wire::cNextChunkOffset, chunkCount_);
}

[[nodiscard]] inline ChunkWriter Segment::claimChunk() noexcept
{
    if (!valid()) {
        return ChunkWriter{};
    }

    std::byte* const base = bytes_.data();
    // The only cross-thread synchronisation on the producer path (R1.3):
    // one relaxed claim on the in-segment cursor -- a fetch_add, or a
    // bounded 32-bit compare-exchange on a core without 64-bit atomics
    // (detail/atomics.hpp).
    const std::uint32_t index = detail::claimChunkIndex(base + wire::cNextChunkOffset, chunkCount_);
    if (index >= chunkCount_) {
        return ChunkWriter{}; // exhausted; caller counts a drop (R9.1).
    }

    std::byte* const chunkBase =
        base + headerBytes_ + static_cast<std::uint64_t>(index) * chunkBytes_;

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
