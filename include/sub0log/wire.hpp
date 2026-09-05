#pragma once

/** @file wire.hpp
 *  @brief The on-disk format. The one contract the producer and the reader
 *         share; neither includes anything of the other.
 *
 *  Layout rationale lives in docs/architecture.md and
 *  docs/framing-and-recovery.md. Rules that code cannot show:
 *   - `formatVersion` sits before everything whose meaning a version bump
 *     could change.
 *   - A record's head word is written *after* its payload, with release
 *     ordering; a reader trusts nothing whose commit tag is absent.
 *   - Every length is bounds-checked against its own container before a
 *     single payload byte is consumed.
 *   - Chunks repeat the segment generation so recycled or stale storage is
 *     rejected on positive evidence, never inferred from zero-fill.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <version>

namespace sub0log::wire {

// The format is little-endian on disk. Big-endian producers byte-swap on
// write (none are first-class targets today; the assert keeps the decision
// visible rather than silent).
static_assert(std::endian::native == std::endian::little,
              "Sub0Log v1 writes little-endian; add byte-swapping before "
              "enabling a big-endian target");

/// "SUB0LOG" + 0x1E (RFC 7464's record separator, as a nod and an
/// improbability). Read as a u64 from offset 0 of a valid segment.
inline constexpr std::uint64_t cMagic = 0x1E'47'4F'4C'30'42'55'53ull; // "SUB0LOG\x1E"

/// Still 1 with continuation chains added, and that is a decision rather
/// than an omission. Version gates *everything*: a decoder that does not
/// recognise it rejects the whole segment (SegmentError::UnknownVersion),
/// which is the right answer only when the older reader would otherwise be
/// misled. It would not be here. A decoder built before chains existed
/// reads a chained segment and gets every Message, with an over-cap
/// argument cut to its first cInlineBytesCap bytes -- exactly what that
/// same decoder would have got from that same call site before chains
/// existed -- and a non-zero Decoder::skippedRecords() telling it there
/// were committed records of a shape it has no reading for. Less data,
/// with the shortfall counted; not wrong data, and not a refused file.
///
/// Bumping would trade that for rejecting the segment outright, which is a
/// worse answer to "my old tool met a new producer" and the opposite of
/// what R3.3 asks for everywhere else in this format: read what you can,
/// count what you cannot.
inline constexpr std::uint16_t cFormatVersion = 1u;

/// Head-word commit tag. Chosen to be non-zero in every byte so a torn
/// 8-byte store is overwhelmingly likely to fail the tag check.
inline constexpr std::uint16_t cCommitTag = 0xC511u;

inline constexpr std::uint32_t cSegmentHeaderBytes = 4096u;
inline constexpr std::uint32_t cDefaultChunkBytes = 64u * 1024u;
inline constexpr std::uint64_t cDefaultSegmentBytes = 8ull * 1024u * 1024u;

/// Records and payloads are 8-byte aligned within a chunk.
inline constexpr std::uint32_t cRecordAlign = 8u;

/// The wire's chunk-size granularity (docs/vnext-adaptive-chunk-sizing.md):
/// every real, declared chunk size is cChunkSizeUnit << (class - 1) for a
/// class in [1, cMaxChunkSizeClass], stamped per chunk
/// (ChunkHeader::chunkSizeClass_) rather than trusted only from the one
/// segment-wide SegmentHeader::chunkBytes_ value. Class 0 is reserved as
/// "unspecified, defer to chunkBytes_" -- which is what every chunk this
/// format ever wrote before this field existed already reads as, since
/// that byte was silent, always-zero reserved space. That is what makes
/// this purely additive: an old reader still never looks at it, and a new
/// reader treats an old chunk's 0 exactly the way it always behaved,
/// needing no format-version gate for either direction.
///
/// 128 is the smallest chunk size already in use anywhere in this
/// codebase (tests/integration/producer.test.cpp's deliberately
/// minimum-viable case) -- picking anything smaller would have meant
/// either breaking that test's intent or widening this field past a
/// single byte for a configuration nothing here has ever actually used.
inline constexpr std::uint32_t cChunkSizeUnit = 128u;
static_assert(cChunkSizeUnit % cRecordAlign == 0u);

/// cChunkSizeUnit << (cMaxChunkSizeClass - 1) is exactly cDefaultSegmentBytes
/// (8 MiB): a chunk configured larger than the whole default segment would
/// leave room for at most one of them, so that is a meaningful ceiling to
/// pick rather than an arbitrary one, with the encoding itself (a full
/// byte) able to represent far more if that default ever changes.
inline constexpr std::uint8_t cMaxChunkSizeClass = 17u;
static_assert((cChunkSizeUnit << (cMaxChunkSizeClass - 1u)) == cDefaultSegmentBytes);

/// Precondition: sizeClass is in [1, cMaxChunkSizeClass]. 0 is the
/// "unspecified" sentinel above and is never a real size -- every caller
/// checks for it first (Segment::claimChunk() never stamps it; reader.hpp's
/// cross-check branches on it before calling this).
[[nodiscard]] constexpr std::uint32_t chunkBytesForSizeClass(const std::uint8_t sizeClass) noexcept
{
    return cChunkSizeUnit << (sizeClass - 1u);
}

/// The reverse of chunkBytesForSizeClass(): the class encoding `bytes`
/// exactly, or std::nullopt if `bytes` is not cChunkSizeUnit << k for any
/// representable k. A cold, once-per-Segment::create() lookup over at most
/// cMaxChunkSizeClass values -- a closed-form bit trick would work just as
/// well but would need to be independently reasoned about here for none of
/// the benefit, since nothing on the emit path ever calls it.
[[nodiscard]] constexpr std::optional<std::uint8_t> sizeClassForChunkBytes(const std::uint32_t bytes) noexcept
{
    for (std::uint8_t sizeClass = 1u; sizeClass <= cMaxChunkSizeClass; ++sizeClass) {
        if (chunkBytesForSizeClass(sizeClass) == bytes) {
            return sizeClass;
        }
    }
    return std::nullopt;
}

/// Inline byte/string payload cap before truncation (per argument).
inline constexpr std::uint16_t cInlineBytesCap = 512u;

/// Continuation-chain cap, **per overflowing argument**: how many further
/// records one Bytes-shaped argument may spill into once it does not fit
/// inline (docs/record-model.md, "Continuation records, for the medium
/// case"). This is the ceiling docs/record-model.md asks for --
/// "capping the chain puts a ceiling on what one log call can cost the
/// calling thread" -- and it is deliberately per-argument rather than
/// per-call: a call site's argument count is fixed at compile time, so
/// bounding each argument independently already bounds the whole call, and
/// it means one long argument's chain length never depends on how many
/// *other* arguments the same call happens to carry.
///
/// Seven, so that an argument reaches cInlineBytesCap * (1 +
/// cMaxContinuations) = 4096 bytes: PATH_MAX on Linux exactly, which is the
/// number to hit when record-model.md says "File paths are what this exists
/// for". An earlier value of four was chosen with the claim that its 2560
/// bytes was "past PATH_MAX (4096)", which is simply false -- it is well
/// short of it, and would have cut precisely the long paths this mechanism
/// is for. The arithmetic is spelled out here so the next person to change
/// the cap checks it rather than inheriting it.
///
/// The cost is at most seven further chunk reservations on the calling
/// thread, for the rare call whose argument is that long; an argument under
/// cInlineBytesCap reserves none of them, and a call site with no
/// Bytes-shaped argument at all never reaches this path (detail/emit.hpp
/// routes on cHasByteArg at compile time). Past the ceiling: truncated,
/// with the cut recorded in cFlagTruncated exactly as an over-cap argument
/// always was (R9.2, visible not silent).
inline constexpr std::uint8_t cMaxContinuations = 7u;

// ---------------------------------------------------------------------------
// Record kinds and flags

enum class RecordKind : std::uint8_t {
    Invalid = 0,       ///< Never written; a zero head word is unwritten space.
    Message = 1,       ///< Varying half of a call site: time, correlation, args.
    SiteDefinition = 2,///< Constant half; precedes the site's first Message.
    Continuation = 3,  ///< Bounded spill of one argument of the Message
                       ///< that opened this chain (see ContinuationPayload).
    Blob = 4,          ///< Reserved, and reviewed in v2 rather than built:
                       ///< a u16 payload already holds kilobytes in one
                       ///< record, and continuation chains already span
                       ///< records, so this would duplicate both
                       ///< (docs/record-model.md, "A blob, for the cold bulk
                       ///< case"). The value stays claimed regardless --
                       ///< kind numbers are stable whether used or not.
    ChildOutput = 5,   ///< One captured line of a child's stdout/stderr (R5.5).
    ChildStart = 6,    ///< A third-party child was spawned: command, correlation.
    ChildExit = 7,     ///< That child ended: exit code or signal.
    SubsystemDefinition = 8, ///< Names one SubsystemId; strict self-description
                             ///< extended to the subsystem axis (docs/record-model.md).
};

enum RecordFlags : std::uint8_t {
    cFlagTruncated = 0x01, ///< A payload was cut at a cap; visible, not silent.
    cFlagContinued = 0x02, ///< The next record (Message or Continuation) is
                           ///< a Continuation still part of this chain --
                           ///< see wire::ContinuationPayload.
};

// ---------------------------------------------------------------------------
// Argument type codes (SiteDefinition carries one per argument)

enum class TypeCode : std::uint8_t {
    Invalid = 0,
    Bool = 1,
    I8 = 2, U8 = 3,
    I16 = 4, U16 = 5,
    I32 = 6, U32 = 7,
    I64 = 8, U64 = 9,
    F32 = 10, F64 = 11,
    Char = 12,
    Bytes = 13,   ///< u16 length, then bytes (string_view, span<byte>).
    Pointer = 14, ///< uintptr_t, logged as an opaque address.
};

/// Wire size of a fixed-size type code; 0 for variable-length (Bytes).
[[nodiscard]] constexpr std::uint32_t fixedSizeOf(const TypeCode code) noexcept
{
    switch (code) {
    case TypeCode::Bool: case TypeCode::I8: case TypeCode::U8: case TypeCode::Char:
        return 1u;
    case TypeCode::I16: case TypeCode::U16:
        return 2u;
    case TypeCode::I32: case TypeCode::U32: case TypeCode::F32:
        return 4u;
    case TypeCode::I64: case TypeCode::U64: case TypeCode::F64: case TypeCode::Pointer:
        return 8u;
    case TypeCode::Bytes: case TypeCode::Invalid:
        return 0u;
    }
    return 0u;
}

// ---------------------------------------------------------------------------
// The record head word
//
// One aligned u64, the only thing a reader validates before deciding whether
// a record exists and how far to advance:
//
//   bits  0..15  payloadBytes  (unpadded; storage advances by paddedPayload)
//   bits 16..23  kind
//   bits 24..31  flags
//   bits 32..47  sequence      (per-chunk ordinal, torn-write evidence)
//   bits 48..63  commitTag     (cCommitTag, or the record does not exist)

struct RecordHead {
    std::uint16_t payloadBytes_{};
    RecordKind kind_{RecordKind::Invalid};
    std::uint8_t flags_{};
    std::uint16_t sequence_{};

    [[nodiscard]] constexpr std::uint64_t pack() const noexcept
    {
        return static_cast<std::uint64_t>(payloadBytes_)
             | (static_cast<std::uint64_t>(kind_) << 16u)
             | (static_cast<std::uint64_t>(flags_) << 24u)
             | (static_cast<std::uint64_t>(sequence_) << 32u)
             | (static_cast<std::uint64_t>(cCommitTag) << 48u);
    }

    /// Unpacks without judging validity; check isCommitted() first.
    [[nodiscard]] static constexpr RecordHead unpack(const std::uint64_t word) noexcept
    {
        return RecordHead{
            static_cast<std::uint16_t>(word & 0xFFFFu),
            static_cast<RecordKind>((word >> 16u) & 0xFFu),
            static_cast<std::uint8_t>((word >> 24u) & 0xFFu),
            static_cast<std::uint16_t>((word >> 32u) & 0xFFFFu),
        };
    }

    [[nodiscard]] static constexpr bool isCommitted(const std::uint64_t word) noexcept
    {
        return static_cast<std::uint16_t>(word >> 48u) == cCommitTag;
    }
};

/// Storage consumed by a payload: padded to record alignment.
[[nodiscard]] constexpr std::uint32_t paddedPayload(const std::uint32_t payloadBytes) noexcept
{
    return (payloadBytes + (cRecordAlign - 1u)) & ~(cRecordAlign - 1u);
}

// ---------------------------------------------------------------------------
// On-disk structures
//
// Written and read by memcpy at fixed offsets; each is asserted trivially
// copyable and its size pinned so a field added carelessly is a compile
// error, not a silent format change.

struct SegmentHeader {
    std::uint64_t magic_;
    std::uint16_t formatVersion_; ///< Before all versioned content (Kafka lesson).
    std::uint16_t reserved0_;
    std::uint32_t headerBytes_;   ///< Offset of chunk 0; geometry is data.
    std::uint32_t chunkBytes_;
    std::uint32_t reserved1_;
    std::uint64_t segmentBytes_;
    std::uint64_t generation_;    ///< Random per segment; every chunk repeats it.
    std::uint64_t processId_;
    std::uint64_t anchorMonoNs_;  ///< Machine-wide monotonic reading ...
    std::uint64_t anchorWallNs_;  ///< ... paired with wall clock (R5.3).
};
static_assert(std::is_trivially_copyable_v<SegmentHeader>);
static_assert(sizeof(SegmentHeader) == 64u);

/// Offset of the chunk-claim cursor within the header page: an atomic u64 on
/// its own cache line, after the readable header fields.
inline constexpr std::uint32_t cNextChunkOffset = 128u;

struct ChunkHeader {
    std::uint64_t generation_;  ///< Must equal SegmentHeader::generation_ (R3.4).
    std::uint64_t ownerThread_; ///< Producer thread id, for R2.2 filtering.
    std::uint64_t claimMonoNs_;
    /// 0 ("unspecified") or a real class -- see cChunkSizeUnit's own comment
    /// for what each means and why this is purely additive. Occupies one
    /// byte of what was, before this field existed, eight bytes of silent
    /// reserved space; the other seven stay reserved.
    std::uint8_t chunkSizeClass_{0};
    std::uint8_t reserved_[7]{};
};
static_assert(std::is_trivially_copyable_v<ChunkHeader>);
static_assert(sizeof(ChunkHeader) == 32u);

// Payload prefixes. Argument bytes follow MessagePayload; the definition's
// variable tail (type codes, then u16-length-prefixed format and file
// strings) follows SiteDefinitionPayload.

struct MessagePayload {
    std::uint64_t siteId_;        ///< Descriptor address in the producing process.
    std::uint64_t monoNs_;
    std::uint64_t correlationId_;
};
static_assert(std::is_trivially_copyable_v<MessagePayload>);
static_assert(sizeof(MessagePayload) == 24u);

/// One slice of an argument's bytes that did not fit its Message record
/// inline (docs/record-model.md, "Continuation records, for the medium
/// case"). Physically the record immediately after the one it continues,
/// in the same chunk: the producer never spreads a chain across chunks
/// (detail/emit.hpp), so a reader is never asked to go looking for one --
/// it either follows straight on from cFlagContinued, or the chain does
/// not exist.
///
/// The chain is self-terminating rather than self-counting: cFlagContinued
/// on *this* record's own head word means "another Continuation
/// immediately follows, still part of the same chain"; its absence ends
/// the chain. This is the same bit the opening Message uses for "at least
/// one Continuation follows", read the same way at every link, so there is
/// no separate part-count to keep in step with it and no way for the
/// count and the records to disagree.
struct ContinuationPayload {
    std::uint64_t siteId_;   ///< The originating Message's site. Not needed
                              ///< to walk the chain (offset order plus the
                              ///< flag already does that) -- this is
                              ///< redundant, on purpose, so a record reached
                              ///< some other way still names what it belongs
                              ///< to, the same role siteId_ plays on Message.
    std::uint8_t argIndex_;  ///< Which of the site's declared arguments
                              ///< (0-based, SiteDefinition order) this
                              ///< slice extends.
    std::uint8_t reserved0_;
    std::uint16_t reserved1_;
    std::uint32_t reserved2_; ///< Explicit tail padding; keeps the size honest.
    // Tail: the next slice of the argument's bytes -- no length prefix, the
    // record's own payloadBytes_ already bounds it (ChildOutputPayload's
    // tail is the same shape for the same reason).
};
static_assert(std::is_trivially_copyable_v<ContinuationPayload>);
static_assert(sizeof(ContinuationPayload) == 16u);

// Child capture (R5.5): a non-cooperating child's output becomes records
// attributed to that child. The three payloads join on childId_ -- a
// parent-side ordinal, not the OS pid, because pids are reused. ChildOutput
// deliberately carries no correlation id: the ChildStart record carries the
// one in scope at spawn, and joining through childId_ is an equality test
// (R6.2) without repeating eight bytes on every captured line.

struct ChildStartPayload {
    std::uint64_t childId_;
    std::uint64_t childPid_;      ///< OS pid, for cross-reference with other tooling.
    std::uint64_t correlationId_; ///< In scope when the child was spawned (R5.5).
    std::uint64_t monoNs_;
    // Tail: u16 commandLen + command-line bytes (truncation flagged).
};
static_assert(std::is_trivially_copyable_v<ChildStartPayload>);
static_assert(sizeof(ChildStartPayload) == 32u);

enum ChildStream : std::uint8_t {
    cChildStdout = 1,
    cChildStderr = 2,
};

struct ChildOutputPayload {
    std::uint64_t childId_;
    std::uint64_t monoNs_;   ///< Capture time in the parent; the child has no clock here.
    std::uint8_t stream_;    ///< ChildStream.
    std::uint8_t reserved0_;
    std::uint16_t reserved1_;
    std::uint32_t reserved2_;
    // Tail: the captured bytes -- one line, newline stripped, no inner
    // length field: the record's payloadBytes already bounds it.
};
static_assert(std::is_trivially_copyable_v<ChildOutputPayload>);
static_assert(sizeof(ChildOutputPayload) == 24u);

struct ChildExitPayload {
    std::uint64_t childId_;
    std::uint64_t monoNs_;
    std::int32_t exitCode_; ///< Meaningful when signal_ is 0.
    std::int32_t signal_;   ///< Terminating signal, 0 if exited normally.
};
static_assert(std::is_trivially_copyable_v<ChildExitPayload>);
static_assert(sizeof(ChildExitPayload) == 24u);

struct SiteDefinitionPayload {
    std::uint64_t siteId_;
    std::uint32_t subsystemId_;
    std::uint32_t line_;
    std::uint8_t severity_;
    std::uint8_t argCount_;
    std::uint16_t reserved0_;
    std::uint32_t reserved1_; ///< Explicit tail padding; keeps the size honest.
    // Tail: argCount_ TypeCode bytes, u16 formatLen + bytes,
    //       u16 fileLen + bytes.
};
static_assert(std::is_trivially_copyable_v<SiteDefinitionPayload>);
static_assert(sizeof(SiteDefinitionPayload) == 24u);

/// Names one SubsystemId, the same way SiteDefinition names one site: an id
/// the producer already carries, plus a length-prefixed string so the
/// segment can be decoded without the table that used to live in a header
/// (docs/record-model.md, "The library must not own the vocabulary" -- the
/// vocabulary is still the consumer's, but its spelling now travels with
/// the segment that used it).
struct SubsystemDefinitionPayload {
    std::uint32_t subsystemId_;
    std::uint16_t nameLen_;
    std::uint16_t reserved0_; ///< Explicit tail padding; keeps the size honest.
    // Tail: nameLen_ bytes, the subsystem's name.
};
static_assert(std::is_trivially_copyable_v<SubsystemDefinitionPayload>);
static_assert(sizeof(SubsystemDefinitionPayload) == 8u);

// ---------------------------------------------------------------------------
// Tiny helpers shared by encoder and decoder

/// memcpy-read a trivially copyable T at p. The caller has bounds-checked.
template <typename T>
[[nodiscard]] inline T loadUnaligned(const std::byte* const p) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

/// memcpy-write a trivially copyable T at p. The caller has bounds-checked.
template <typename T>
inline void storeUnaligned(std::byte* const p, const T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(p, &value, sizeof(T));
}

/** Begins a `uint64_t`'s lifetime at `p` and returns a pointer to it, for
 *  the two call sites (`ChunkWriter::commit`, `Segment::claimChunk`) that
 *  hand a word inside `mmap`/`MapViewOfFile` storage to `atomic_ref`.
 *  `atomic_ref` does not create the object it wraps -- its precondition is
 *  that the referenced object already exists ([atomics.ref.generic]) -- and
 *  a raw `reinterpret_cast<uint64_t*>` dereferenced to form that reference
 *  is only defined once an object of that type occupies the storage
 *  ([basic.life]). Nothing upstream of these two sites performs one of the
 *  operations the object model recognises as implicitly creating an object
 *  (`malloc`, `memcpy`, and the short fixed list P0593 names) at either
 *  address, so forming the reference directly is technically undefined --
 *  the same question `loadUnaligned`/`storeUnaligned` above sidestep by
 *  going through `memcpy`, which cannot serve here because `atomic_ref`
 *  needs a live object to wrap, not a copy of one.
 *
 *  `std::start_lifetime_as` (C++23, P2590) is that operation, compiling to
 *  nothing beyond the `reinterpret_cast` it replaces. It is conditionally
 *  compiled rather than used unconditionally because it is not yet what
 *  every compiler on this project's own floor actually ships: absent from
 *  both `<version>`'s `__cpp_lib_start_lifetime_as` and a direct compile
 *  probe on GCC 13 and Clang 18, the two compilers `linux-gcc`/
 *  `linux-clang` build against. The fallback is the `reinterpret_cast` this
 *  function replaces where the real operation is unavailable -- unsound by
 *  the same strict reading, not by anything narrower, but consistent with
 *  what every mainstream compiler has always done with `mmap`-obtained
 *  storage in practice, so this is a paper cut against a future compiler
 *  that starts exploiting the gap, not a fix for an observed one.
 *
 *  The caller must have already checked the address is 8-byte aligned;
 *  neither branch does so itself.
 */
[[nodiscard]] inline std::uint64_t* startUint64LifetimeAt(std::byte* const p) noexcept
{
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
    return std::start_lifetime_as<std::uint64_t>(p);
#else
    return reinterpret_cast<std::uint64_t*>(p);
#endif
}

} // namespace sub0log::wire
