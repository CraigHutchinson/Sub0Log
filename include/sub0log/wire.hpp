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
#include <type_traits>

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

inline constexpr std::uint16_t cFormatVersion = 1u;

/// Head-word commit tag. Chosen to be non-zero in every byte so a torn
/// 8-byte store is overwhelmingly likely to fail the tag check.
inline constexpr std::uint16_t cCommitTag = 0xC511u;

inline constexpr std::uint32_t cSegmentHeaderBytes = 4096u;
inline constexpr std::uint32_t cDefaultChunkBytes = 64u * 1024u;
inline constexpr std::uint64_t cDefaultSegmentBytes = 8ull * 1024u * 1024u;

/// Records and payloads are 8-byte aligned within a chunk.
inline constexpr std::uint32_t cRecordAlign = 8u;

/// Inline byte/string payload cap before truncation (per argument).
inline constexpr std::uint16_t cInlineBytesCap = 512u;

/// Continuation-chain cap: the ceiling on what one call can cost (see
/// docs/record-model.md). Reserved in v1; the writer truncates instead.
inline constexpr std::uint8_t cMaxContinuations = 4u;

// ---------------------------------------------------------------------------
// Record kinds and flags

enum class RecordKind : std::uint8_t {
    Invalid = 0,       ///< Never written; a zero head word is unwritten space.
    Message = 1,       ///< Varying half of a call site: time, correlation, args.
    SiteDefinition = 2,///< Constant half; precedes the site's first Message.
    Continuation = 3,  ///< Bounded spill of the preceding record's payload.
    Blob = 4,          ///< Reserved: cold bulk payloads (v2).
    ChildOutput = 5,   ///< One captured line of a child's stdout/stderr (R5.5).
    ChildStart = 6,    ///< A third-party child was spawned: command, correlation.
    ChildExit = 7,     ///< That child ended: exit code or signal.
    SubsystemDefinition = 8, ///< Names one SubsystemId; strict self-description
                             ///< extended to the subsystem axis (docs/record-model.md).
};

enum RecordFlags : std::uint8_t {
    cFlagTruncated = 0x01, ///< A payload was cut at a cap; visible, not silent.
    cFlagContinued = 0x02, ///< A Continuation record follows this one.
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
    std::uint64_t reserved_;
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

} // namespace sub0log::wire
