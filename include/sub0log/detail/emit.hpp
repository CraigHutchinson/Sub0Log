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
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

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
