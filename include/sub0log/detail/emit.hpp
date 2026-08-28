#pragma once

/** @file detail/emit.hpp
 *  @brief The producer emit path: definition-on-first-use, then the message
 *         record. Called only from the call-site macros in log.hpp.
 */

#include "../context.hpp"
#include "../encode.hpp"
#include "../instance.hpp"
#include "../site.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace sub0log::detail {

/// The macro's enabled check: one relaxed load of the active instance and
/// one of its threshold (R1.4).
///
/// Nothing bound is not the same answer as below threshold, even though
/// both return false here: the first means this call site is emitting into
/// nowhere and nobody would otherwise learn of it (R9.3), so it is counted
/// on the way out. The counter is on the null branch only -- a bound
/// instance never touches it.
[[nodiscard]] inline bool enabled(const Severity severity,
                                  const SubsystemId subsystem) noexcept
{
    Logger* const logger = Logger::active();
    if (logger == nullptr) [[unlikely]] {
        return countUnboundEmit();
    }
    return atLeast(severity, logger->threshold(subsystem));
}

// ---------------------------------------------------------------------------
// SiteDefinition: written once per site per *segment*, before that site's
// first Message in it (docs/record-model.md, "strict self-description").
// Per segment rather than per process, because a segment that does not
// carry the definitions for its own records cannot be decoded alone.

/// Bytes needed for a SiteDefinition payload carrying `argCount` type codes
/// and format/file strings of the given (possibly already-clamped) lengths.
[[nodiscard]] constexpr std::uint32_t definitionPayloadBytes(std::uint32_t argCount,
                                                              std::size_t formatLen,
                                                              std::size_t fileLen) noexcept
{
    return static_cast<std::uint32_t>(sizeof(wire::SiteDefinitionPayload)) + argCount
         + 2u + static_cast<std::uint32_t>(formatLen)
         + 2u + static_cast<std::uint32_t>(fileLen);
}

/** The non-template core of a SiteDefinition write: siteId, subsystem,
 *  severity, the two strings, and the wire bytes for the argument type
 *  codes -- every field the record carries, addressed by value rather than
 *  read off a SiteDescriptor.
 *
 *  Two callers share this rather than two copies of the record layout: the
 *  template below derives `typeCodes` from a pack of C++ types and reads
 *  everything else off a SiteDescriptor; abi_host.hpp's `define_site` has
 *  neither of those -- a plugin call site is described entirely by the
 *  arguments the C ABI carries -- so it calls this directly with the bytes
 *  the plugin already supplied. `typeCodes` is `uint8_t` rather than
 *  `wire::TypeCode` for the same reason: the ABI hands over a `const
 *  uint8_t*` whose bytes were never constructed as `wire::TypeCode` objects,
 *  and reading them through a reinterpret_cast to the enum type would be
 *  exactly the aliasing violation `wire::loadUnaligned` exists to avoid
 *  elsewhere in this codebase -- so the shared core takes what both callers
 *  can honestly provide, and interpretation as TypeCode stays the reader's
 *  job, same as it already is for every other byte on the wire.
 *
 *  Called only when this segment has not been told about the site yet; the
 *  caller records the segment's generation (relaxed, or in the ABI's case,
 *  in its own site table -- see abi_host.hpp) after this returns, so the
 *  definition is guaranteed to precede every Message written for the site
 *  in that segment. A duplicate under a first-use race is benign -- a
 *  reader keeps the first one it sees.
 *  Returns false when the definition could not be written (the drop is
 *  counted). The caller must NOT then record the generation: doing so
 *  marks the site announced for a segment that never learned it, and every
 *  later Message for that site decodes as undecodable -- silently, which
 *  is the precise failure keying on the generation exists to prevent.
 */
[[nodiscard]] inline bool writeSiteDefinitionCore(Logger& logger, std::uint64_t siteId,
                                                  SubsystemId subsystem, Severity severity,
                                                  std::string_view format, std::string_view file,
                                                  std::uint32_t line,
                                                  std::span<const std::uint8_t> typeCodes) noexcept
{
    const auto argCount = static_cast<std::uint32_t>(typeCodes.size());
    std::size_t formatLen = format.size();
    std::size_t fileLen = file.size();
    bool truncated = false;

    ChunkWriter* writer = logger.currentWriter();
    ChunkWriter::Reservation slot =
        (writer != nullptr)
            ? writer->reserve(definitionPayloadBytes(argCount, formatLen, fileLen))
            : ChunkWriter::Reservation{};

    if (!slot.valid()) {
        writer = logger.refillWriter();
        if (writer == nullptr) {
            logger.countDrop();
            return false;
        }
        slot = writer->reserve(definitionPayloadBytes(argCount, formatLen, fileLen));
        if (!slot.valid()) {
            // Doesn't fit even a brand-new, empty chunk: truncate the two
            // strings so it does, rather than dropping the definition
            // outright (a missing definition makes every Message for this
            // site undecodable, which is worse than a shortened file name).
            // Visible via cFlagTruncated, never silent (R9.2).
            // Two ceilings, not one. Chunk capacity is the obvious limit;
            // the head word's 16-bit payload length is the one that bites
            // with large chunks, where a long format plus a long __FILE__
            // can exceed 0xFFFF while fitting the chunk easily. Missing it
            // meant reserve() refused forever, and every emit for the site
            // burned a fresh chunk and wrote nothing.
            constexpr std::uint32_t cMaxPayload = 0xFFFFu;
            const std::uint32_t emptyCapacity =
                std::min(writer->remaining(), cMaxPayload);
            const std::uint32_t fixedBytes =
                static_cast<std::uint32_t>(sizeof(wire::SiteDefinitionPayload))
                + argCount + 4u /* two u16 lengths */ + 8u /* head word */;
            const std::uint32_t available = emptyCapacity > fixedBytes ? emptyCapacity - fixedBytes : 0u;
            const std::uint32_t formatCap = available / 2u;
            const std::uint32_t fileCap = available - formatCap;
            if (formatLen > formatCap) {
                formatLen = formatCap;
                truncated = true;
            }
            if (fileLen > fileCap) {
                fileLen = fileCap;
                truncated = true;
            }
            slot = writer->reserve(definitionPayloadBytes(argCount, formatLen, fileLen));
            if (!slot.valid()) {
                logger.countDrop();
                return false;
            }
        }
    }

    std::byte* p = slot.payload_;

    wire::SiteDefinitionPayload defPayload{};
    defPayload.siteId_ = siteId;
    defPayload.subsystemId_ = subsystem.value_;
    defPayload.line_ = line;
    defPayload.severity_ = static_cast<std::uint8_t>(severity);
    defPayload.argCount_ = static_cast<std::uint8_t>(argCount);
    defPayload.reserved0_ = 0u;
    defPayload.reserved1_ = 0u;
    wire::storeUnaligned(p, defPayload);
    p += sizeof(wire::SiteDefinitionPayload);

    for (std::size_t i = 0; i < typeCodes.size(); ++i) {
        wire::storeUnaligned(p, typeCodes[i]);
        p += 1;
    }

    wire::storeUnaligned(p, static_cast<std::uint16_t>(formatLen));
    p += 2;
    if (formatLen > 0u) {
        std::memcpy(p, format.data(), formatLen);
    }
    p += formatLen;

    wire::storeUnaligned(p, static_cast<std::uint16_t>(fileLen));
    p += 2;
    if (fileLen > 0u) {
        std::memcpy(p, file.data(), fileLen);
    }
    p += fileLen;

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(p - slot.payload_);
    head.kind_ = wire::RecordKind::SiteDefinition;
    head.flags_ = truncated ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};

    writer->commit(slot, head);
    if (truncated) {
        logger.countTruncation();
    }
    return true;
}

/// Writes site's SiteDefinition record by deriving the wire type codes from
/// the template pack and reading everything else off the SiteDescriptor;
/// see writeSiteDefinitionCore for the shared layout and the full contract
/// (when this may be called, and what the caller must and must not do with
/// its result).
template <Encodable... Args>
[[nodiscard]] bool writeSiteDefinition(Logger& logger, const SiteDescriptor& site) noexcept
{
    constexpr std::size_t cArgCount = sizeof...(Args);
    // Zero-size arrays are not a thing; writeSiteDefinitionCore never reads
    // the padding slot when cArgCount is 0 (its span length is 0).
    const std::uint8_t typeCodes[cArgCount == 0 ? 1 : cArgCount] = {
        static_cast<std::uint8_t>(typeCodeFor<Args>())...};

    return writeSiteDefinitionCore(logger, site.id(), site.subsystem_, site.severity_,
                                   site.format_, site.file_, site.line_,
                                   std::span<const std::uint8_t>{typeCodes, cArgCount});
}

// ---------------------------------------------------------------------------
// Continuation chains (docs/record-model.md, "Continuation records, for the
// medium case"). Everything in this section is reached only from emitChained
// below, which emit() only calls `if constexpr (cHasByteArg<Args...>)` --
// a call site whose pack is all fixed-size arguments never instantiates any
// of it, so it costs that call site nothing (not even a branch).

/// One argument's overflow past the inline cap: the bytes still to place
/// into continuation records, and which of the site's declared arguments
/// they belong to (wire::ContinuationPayload::argIndex_).
struct ArgOverflow {
    const std::byte* tail_{nullptr}; ///< First overflow byte (base + inline length).
    std::size_t length_{0};          ///< Bytes still to place, already clamped
                                      ///< to what wire::cMaxContinuations allows.
    std::uint8_t argIndex_{0};
};

/// One outstanding Continuation reservation, and what to write into it once
/// the whole chain's reservations have succeeded. Nothing is written before
/// that: a slice memcpy'd into a slot that the chain then fails to complete
/// is inert (its head word is never committed, so a reader sees unwritten
/// space, not a record -- chunk.hpp), but writing it twice into two
/// different chunks on a retry would not be, so source_/length_/argIndex_
/// are kept until the group's fate is decided rather than written eagerly.
struct ContinuationSlot {
    ChunkWriter::Reservation reservation_{};
    const std::byte* source_{nullptr};
    std::uint32_t length_{0};
    std::uint8_t argIndex_{0};
};

/// Bytes one record occupies in a chunk: its head word plus its padded
/// payload. Chain sizing needs this before reserving anything -- see
/// reserveChain.
[[nodiscard]] inline constexpr std::uint64_t recordFootprint(const std::uint32_t payloadBytes) noexcept
{
    return 8u + static_cast<std::uint64_t>(wire::paddedPayload(payloadBytes));
}

/// Reserves the Message record plus one Continuation per
/// wire::cInlineBytesCap-sized slice of every entry in `overflows`, all
/// from `writer`'s current chunk -- a chain never spans chunks (the same
/// "in the same thread's chunk" docs/record-model.md promises).
///
/// All or nothing, and that is load-bearing rather than tidy. An earlier
/// version reserved until one call failed and left the successful ones
/// abandoned, on the reasoning that an uncommitted head word reads as
/// unwritten space. It does -- and SegmentReader::visit *stops* at the
/// first one, so an abandoned reservation with a committed record after it
/// hides that record and every later record in the chunk, counted as
/// unwritten rather than as damage. Silent loss, in the mechanism whose
/// whole point is not having any. So the fit is decided by arithmetic
/// against remaining() before a single byte of the chunk is claimed, and a
/// chunk that cannot take the whole chain is left exactly as it was.
///
/// Nothing is committed here -- see emitChained for why (commit order, not
/// reserve order, is what a reader may observe).
template <std::size_t MaxSlots>
[[nodiscard]] inline bool reserveChain(ChunkWriter& writer, const std::uint32_t messagePayloadBytes,
                                       std::span<const ArgOverflow> overflows,
                                       ChunkWriter::Reservation& messageSlot,
                                       std::array<ContinuationSlot, MaxSlots>& slots,
                                       std::size_t& slotCount) noexcept
{
    std::uint64_t needed = recordFootprint(messagePayloadBytes);
    for (const ArgOverflow& overflow : overflows) {
        std::size_t remaining = overflow.length_;
        while (remaining > 0u) {
            const auto sliceLen = std::min<std::size_t>(remaining, wire::cInlineBytesCap);
            needed += recordFootprint(
                static_cast<std::uint32_t>(sizeof(wire::ContinuationPayload) + sliceLen));
            remaining -= sliceLen;
        }
    }
    if (needed > writer.remaining()) {
        return false;
    }

    messageSlot = writer.reserve(messagePayloadBytes);
    if (!messageSlot.valid()) {
        return false;
    }
    slotCount = 0;
    for (const ArgOverflow& overflow : overflows) {
        std::size_t remaining = overflow.length_;
        const std::byte* source = overflow.tail_;
        while (remaining > 0u) {
            const auto sliceLen =
                static_cast<std::uint32_t>(std::min<std::size_t>(remaining, wire::cInlineBytesCap));
            const auto payloadBytes =
                static_cast<std::uint32_t>(sizeof(wire::ContinuationPayload)) + sliceLen;
            const ChunkWriter::Reservation slot = writer.reserve(payloadBytes);
            if (!slot.valid() || slotCount >= MaxSlots) {
                // Unreachable given the arithmetic above and MaxSlots being
                // sized from the same ceiling the overflow lengths were
                // clamped to -- checked anyway, because the alternative to
                // an impossible early return is an out-of-bounds write.
                return false;
            }
            slots[slotCount] = ContinuationSlot{slot, source, sliceLen, overflow.argIndex_};
            ++slotCount;
            source += sliceLen;
            remaining -= sliceLen;
        }
    }
    return true;
}

/** The continuation-aware Message path: reached only when the argument pack
 *  contains at least one Bytes-shaped argument (emit() decides this with
 *  `if constexpr`). Everything below reduces to exactly what emit()'s plain
 *  path already did whenever no argument actually overflows the inline cap
 *  at runtime -- one reservation, one commit, no continuations -- so a
 *  short string argument pays for the extra scan this function does but not
 *  for a chain it never needed.
 *
 *  Shape on the wire: the Message record is unchanged (site.id(), time,
 *  correlation, then each argument -- fixed raw, Bytes as u16 length then
 *  up to wire::cInlineBytesCap bytes, exactly as encodeArgs always wrote
 *  it), so a reader that has never heard of Continuation still decodes
 *  every argument to at least its first cInlineBytesCap bytes. What is new
 *  is what follows a Message whose flags carry cFlagContinued: one
 *  Continuation per further slice of every overflowing argument, argument
 *  by argument, each naming which argument it extends
 *  (wire::ContinuationPayload::argIndex_) and each carrying cFlagContinued
 *  itself except the last, which is what lets a reader walk the chain to
 *  its end without needing a separate count.
 *
 *  Commit order is the one thing this function is careful about, twice:
 *  every Continuation commits *before* the Message that names it (see
 *  chunk.hpp's ChunkWriter comment on commit-vs-reserve order, and
 *  SegmentReader::visit's stop-at-the-first-uncommitted-word walk) -- so a
 *  reader arriving mid-write, live-tail or otherwise, can only ever observe
 *  the whole chain or none of it, never a Message whose promised tail is
 *  not there yet. And nothing commits at all until every reservation the
 *  chain needs has succeeded, in the *same* chunk (record-model.md: "in the
 *  same thread's chunk") -- a chain is never split across chunks, so it is
 *  never partially published across one either.
 *
 *  Fit handling, tried in order: the current chunk; a freshly claimed one,
 *  if the current one is spent; and, if even an empty chunk cannot hold the
 *  whole chain (a small custom chunkBytes against a wide argument list can
 *  do this), the flat truncate-at-cInlineBytesCap this argument always got
 *  before continuations existed, rather than publish a partial chain or
 *  drop the record outright. Every one of these keeps cFlagTruncated
 *  meaning what it always meant: bytes were actually lost, visible in the
 *  record's own flags (R9.2).
 */
template <Encodable... Args>
void emitChained(Logger& logger, const SiteDescriptor& site, const Args&... args) noexcept
{
    constexpr std::size_t cArgCount = sizeof...(Args);
    constexpr std::size_t cMaxSlots = cArgCount * static_cast<std::size_t>(wire::cMaxContinuations);

    // One scan per argument -- the only place a CStringLike argument's true
    // length is measured on this path (rawBytesOf), cached here so sizing
    // and writing both read it rather than re-scanning (encodeArgs would
    // otherwise be a second scan of the same const char*).
    // Left uninitialised on purpose, and safe only because every element
    // is written before it is read: the folds below cover the pack exactly
    // once each (including the fixed-argument branch, which writes zeros
    // rather than relying on an initialiser), and `slots` is only ever read
    // below slotCount. `slots` is the one that matters -- it is
    // cArgCount * wire::cMaxContinuations entries, so value-initialising it
    // would memset a kilobyte on a call site whose string argument is
    // sixteen bytes long and needs none of it.
    std::array<std::pair<const void*, std::size_t>, cArgCount> raw;
    {
        std::size_t i = 0;
        auto scanOne = [&]<typename T>(const T& value) { raw[i++] = rawBytesOf<T>(value); };
        (scanOne(args), ...);
    }

    std::array<std::uint32_t, cArgCount> inlineLen;
    std::array<std::size_t, cArgCount> overflowLen;
    std::uint32_t argsBytes = 0;
    bool anyTruncated = false;
    {
        std::size_t i = 0;
        auto sizeOne = [&]<typename T>(const T&) {
            if constexpr (ByteView<T> || CStringLike<T> || StringViewLike<T>) {
                // rawLen is the true length for a view (its size() is
                // already O(1)) and, for a C string, a length bounded at
                // cArgCeiling + 1 by rawBytesOf -- enough to tell "longer
                // than the ceiling" from "exactly at it" without an
                // unbounded scan (see rawBytesOf's own comment).
                std::size_t rawLen = raw[i].second;
                if (rawLen > cArgCeiling) {
                    rawLen = cArgCeiling; // past the ceiling: real, visible loss.
                    anyTruncated = true;
                }
                const auto inl =
                    static_cast<std::uint32_t>(std::min<std::size_t>(rawLen, wire::cInlineBytesCap));
                inlineLen[i] = inl;
                overflowLen[i] = rawLen > inl ? rawLen - inl : 0u;
                argsBytes += 2u + inl;
            } else {
                // A fixed-size argument has no inline or overflow length,
                // and the loop below reads both for *every* argument -- so
                // they are written here rather than left to the array's
                // initialiser. That is not tidiness: when these two arrays
                // were left uninitialised, a fixed argument's garbage
                // overflowLen read as an overflow, and the chain builder
                // took raw[i].first -- null for a fixed argument -- as the
                // base of a memcpy. Correctness that depends on a distant
                // `{}` is correctness waiting to be optimised away.
                inlineLen[i] = 0u;
                overflowLen[i] = 0u;
                argsBytes += fixedWireSize<T>();
            }
            ++i;
        };
        (sizeOne(args), ...);
    }

    std::array<ArgOverflow, cArgCount> overflowsArr{};
    std::size_t overflowCount = 0;
    for (std::size_t i = 0; i < cArgCount; ++i) {
        if (overflowLen[i] > 0u) {
            const auto* base = static_cast<const std::byte*>(raw[i].first);
            overflowsArr[overflowCount] = ArgOverflow{base + inlineLen[i], overflowLen[i],
                                                       static_cast<std::uint8_t>(i)};
            ++overflowCount;
        }
    }
    const std::span<const ArgOverflow> overflows{overflowsArr.data(), overflowCount};

    const auto messagePayloadBytes =
        static_cast<std::uint32_t>(sizeof(wire::MessagePayload)) + argsBytes;

    ChunkWriter::Reservation msgSlot{};
    std::array<ContinuationSlot, cMaxSlots> slots;
    std::size_t slotCount = 0;

    ChunkWriter* writer = logger.currentWriter();
    bool reserved = writer != nullptr &&
        reserveChain(*writer, messagePayloadBytes, overflows, msgSlot, slots, slotCount);
    if (!reserved) {
        writer = logger.refillWriter();
        reserved = writer != nullptr &&
            reserveChain(*writer, messagePayloadBytes, overflows, msgSlot, slots, slotCount);
    }
    if (!reserved) {
        if (writer == nullptr) {
            logger.countDrop();
            return;
        }
        // Even a brand-new, empty chunk cannot hold the whole chain. Never
        // publish a partial one (the commit-order contract above exists so
        // a reader can never observe that) -- fall back to the record's own
        // cap alone, exactly as if continuations did not exist, rather than
        // drop the record outright.
        slotCount = 0;
        anyTruncated = true;
        msgSlot = writer->reserve(messagePayloadBytes);
        if (!msgSlot.valid()) {
            logger.countDrop();
            return;
        }
    }

    const std::uint64_t scoped = currentCorrelation();
    const std::uint64_t correlation = scoped != 0u ? scoped : logger.rootCorrelation();
    wire::storeUnaligned(msgSlot.payload_,
                         wire::MessagePayload{site.id(), monotonicNowNs(), correlation});

    std::byte* msgPtr = msgSlot.payload_ + sizeof(wire::MessagePayload);
    {
        std::size_t i = 0;
        auto writeOne = [&]<typename T>(const T& value) {
            if constexpr (ByteView<T> || CStringLike<T> || StringViewLike<T>) {
                wire::storeUnaligned(msgPtr, static_cast<std::uint16_t>(inlineLen[i]));
                msgPtr += 2;
                if (inlineLen[i] > 0u) {
                    std::memcpy(msgPtr, raw[i].first, inlineLen[i]);
                }
                msgPtr += inlineLen[i];
            } else {
                msgPtr += encodeFixedOne(msgPtr, value);
            }
            ++i;
        };
        (writeOne(args), ...);
    }

    for (std::size_t s = 0; s < slotCount; ++s) {
        const ContinuationSlot& cont = slots[s];
        wire::ContinuationPayload prefix{};
        prefix.siteId_ = site.id();
        prefix.argIndex_ = cont.argIndex_;
        wire::storeUnaligned(cont.reservation_.payload_, prefix);
        if (cont.length_ > 0u) {
            std::memcpy(cont.reservation_.payload_ + sizeof(wire::ContinuationPayload),
                       cont.source_, cont.length_);
        }
    }

    // Commit last-to-first: every Continuation becomes visible before the
    // Message that names it does, so a reader can never observe a Message
    // whose chain is promised (cFlagContinued) but not yet there.
    for (std::size_t s = 0; s < slotCount; ++s) {
        wire::RecordHead contHead{};
        contHead.payloadBytes_ =
            static_cast<std::uint16_t>(sizeof(wire::ContinuationPayload) + slots[s].length_);
        contHead.kind_ = wire::RecordKind::Continuation;
        contHead.flags_ = (s + 1u < slotCount) ? static_cast<std::uint8_t>(wire::cFlagContinued)
                                               : std::uint8_t{0};
        writer->commit(slots[s].reservation_, contHead);
    }

    wire::RecordHead msgHead{};
    msgHead.payloadBytes_ = static_cast<std::uint16_t>(messagePayloadBytes);
    msgHead.kind_ = wire::RecordKind::Message;
    std::uint8_t flags = anyTruncated ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};
    if (slotCount > 0u) {
        flags |= wire::cFlagContinued;
    }
    msgHead.flags_ = flags;
    writer->commit(msgSlot, msgHead);

    if (anyTruncated) {
        logger.countTruncation();
    }
}

// ---------------------------------------------------------------------------
// Message

/** Writes, into the active logger's current chunk:
 *   - the site's SiteDefinition record, iff this segment has not been told
 *     about the site yet (one relaxed load and a comparison; the segment's
 *     generation is recorded after the definition commits) -- a definition
 *     always precedes the site's first message in that segment;
 *   - the Message record: MessagePayload{site.id(), monotonicNowNs(),
 *     currentCorrelation()} then encodeArgs(args...).
 *
 *  Fit handling: a record that does not fit the current chunk's remainder
 *  claims a fresh chunk; if none, the drop is counted and the call returns
 *  (R9.1). Truncation by encodeArgs sets cFlagTruncated and is counted.
 *
 *  A Bytes-shaped argument that overflows the inline cap is handled by
 *  emitChained instead (below the `if constexpr`): this function's own
 *  body, for a pack with no such argument, is untouched by that -- it
 *  compiles exactly as it did before continuation chains existed, which is
 *  the whole of what protects emit.fixed2/emit.disabled's measured cost
 *  (benchmarks/emit.bench.cpp).
 *
 *  No allocation, no lock, no formatting, noexcept end to end (R1.1-R1.3).
 */
template <Encodable... Args>
void emit(const SiteDescriptor& site, const Args&... args) noexcept
{
    Logger* const logger = Logger::active();
    if (logger == nullptr) {
        return; // enabled() already gates the macro; defensive only.
    }

    // Strict self-description is per *segment*: every segment must carry the
    // definitions for the sites it contains, or its records cannot be
    // decoded on their own (R4.3, and docs/record-model.md). Comparing the
    // site's last-announced generation against this segment's is what makes
    // that true when a process outlives a Logger.
    const std::uint64_t generation = logger->segmentGeneration();
    if (site.announcedGeneration_.load(std::memory_order_relaxed) != generation) {
        if (!writeSiteDefinition<Args...>(*logger, site)) {
            // The definition did not make it, so this segment still does not
            // know the site. Leaving the generation unrecorded means the next
            // emit tries again; recording it would strand every later Message
            // for this site as undecodable.
            //
            // No countDrop() here: writeSiteDefinition already counted the
            // one it could not write. This call loses exactly one record, so
            // it must contribute exactly one drop -- the stress harness
            // asserts emitted == decoded + dropped, and a double count makes
            // that ledger lie in the direction that looks safe.
            return;
        }
        site.announcedGeneration_.store(generation, std::memory_order_relaxed);
    }

    if constexpr (cHasByteArg<Args...>) {
        emitChained(*logger, site, args...);
        return;
    }

    const std::uint32_t argsBytes = upperBoundSize(args...);
    const std::uint32_t payloadBytes =
        static_cast<std::uint32_t>(sizeof(wire::MessagePayload)) + argsBytes;

    ChunkWriter* writer = nullptr;
    const ChunkWriter::Reservation slot = reserveRecord(*logger, payloadBytes, writer);
    if (!slot.valid()) {
        return; // reserveRecord counted the drop.
    }

    // A thread with no CorrelationScope active still belongs to whatever
    // activity spawned this process (R5.4): fall back to the instance root.
    const std::uint64_t scoped = currentCorrelation();
    const std::uint64_t correlation = scoped != 0u ? scoped : logger->rootCorrelation();
    const wire::MessagePayload messagePayload{site.id(), monotonicNowNs(), correlation};
    wire::storeUnaligned(slot.payload_, messagePayload);

    const EncodeResult encoded =
        encodeArgs(slot.payload_ + sizeof(wire::MessagePayload),
                  payloadBytes - static_cast<std::uint32_t>(sizeof(wire::MessagePayload)), args...);

    wire::RecordHead head{};
    head.payloadBytes_ =
        static_cast<std::uint16_t>(sizeof(wire::MessagePayload) + encoded.bytes_);
    head.kind_ = wire::RecordKind::Message;
    head.flags_ = encoded.truncated_ ? static_cast<std::uint8_t>(wire::cFlagTruncated) : std::uint8_t{0};

    writer->commit(slot, head);

    if (encoded.truncated_) {
        logger->countTruncation();
    }
}

} // namespace sub0log::detail
