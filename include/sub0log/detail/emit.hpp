#pragma once

/** @file detail/emit.hpp
 *  @brief The producer emit path: definition-on-first-use, then the message
 *         record. Called only from the call-site macros in log.hpp.
 */

#include "../context.hpp"
#include "../encode.hpp"
#include "../instance.hpp"
#include "../site.hpp"

#include <cstring>

namespace sub0log::detail {

/// The macro's enabled check: one relaxed load of the active instance and
/// one of its threshold (R1.4). False when nothing is bound.
[[nodiscard]] inline bool enabled(const Severity severity) noexcept
{
    Logger* const logger = Logger::active();
    return logger != nullptr && atLeast(severity, logger->threshold());
}

// ---------------------------------------------------------------------------
// SiteDefinition: written once per site per process, before that site's
// first Message (docs/record-model.md, "strict self-description").

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

/// Writes site's SiteDefinition record. Only called while site.announced_
/// is still 0; the caller sets it to 1 (relaxed) after this returns, so the
/// definition is guaranteed to precede every Message this process writes
/// for the site (a duplicate under a first-use race is benign -- a reader
/// keeps the first one it sees).
template <Encodable... Args>
void writeSiteDefinition(Logger& logger, const SiteDescriptor& site) noexcept
{
    constexpr std::size_t cArgCount = sizeof...(Args);
    // Zero-size arrays are not a thing; the loop below never touches the
    // padding slot when cArgCount is 0.
    const wire::TypeCode typeCodes[cArgCount == 0 ? 1 : cArgCount] = {typeCodeFor<Args>()...};

    std::size_t formatLen = site.format_.size();
    std::size_t fileLen = site.file_.size();
    bool truncated = false;

    ChunkWriter* writer = logger.currentWriter();
    ChunkWriter::Reservation slot =
        (writer != nullptr)
            ? writer->reserve(definitionPayloadBytes(cArgCount, formatLen, fileLen))
            : ChunkWriter::Reservation{};

    if (!slot.valid()) {
        writer = logger.refillWriter();
        if (writer == nullptr) {
            logger.countDrop();
            return;
        }
        slot = writer->reserve(definitionPayloadBytes(cArgCount, formatLen, fileLen));
        if (!slot.valid()) {
            // Doesn't fit even a brand-new, empty chunk: truncate the two
            // strings so it does, rather than dropping the definition
            // outright (a missing definition makes every Message for this
            // site undecodable, which is worse than a shortened file name).
            // Visible via cFlagTruncated, never silent (R9.2).
            const std::uint32_t emptyCapacity = writer->remaining();
            const std::uint32_t fixedBytes =
                static_cast<std::uint32_t>(sizeof(wire::SiteDefinitionPayload))
                + static_cast<std::uint32_t>(cArgCount) + 4u /* two u16 lengths */ + 8u /* head word */;
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
            slot = writer->reserve(definitionPayloadBytes(cArgCount, formatLen, fileLen));
            if (!slot.valid()) {
                logger.countDrop();
                return;
            }
        }
    }

    std::byte* p = slot.payload_;

    wire::SiteDefinitionPayload defPayload{};
    defPayload.siteId_ = site.id();
    defPayload.subsystemId_ = site.subsystem_.value_;
    defPayload.line_ = site.line_;
    defPayload.severity_ = static_cast<std::uint8_t>(site.severity_);
    defPayload.argCount_ = static_cast<std::uint8_t>(cArgCount);
    defPayload.reserved0_ = 0u;
    defPayload.reserved1_ = 0u;
    wire::storeUnaligned(p, defPayload);
    p += sizeof(wire::SiteDefinitionPayload);

    for (std::size_t i = 0; i < cArgCount; ++i) {
        wire::storeUnaligned(p, static_cast<std::uint8_t>(typeCodes[i]));
        p += 1;
    }

    wire::storeUnaligned(p, static_cast<std::uint16_t>(formatLen));
    p += 2;
    if (formatLen > 0u) {
        std::memcpy(p, site.format_.data(), formatLen);
    }
    p += formatLen;

    wire::storeUnaligned(p, static_cast<std::uint16_t>(fileLen));
    p += 2;
    if (fileLen > 0u) {
        std::memcpy(p, site.file_.data(), fileLen);
    }
    p += fileLen;

    wire::RecordHead head{};
    head.payloadBytes_ = static_cast<std::uint16_t>(p - slot.payload_);
    head.kind_ = wire::RecordKind::SiteDefinition;
    head.flags_ = truncated ? wire::cFlagTruncated : std::uint8_t{0};

    writer->commit(slot, head);
    if (truncated) {
        logger.countTruncation();
    }
}

// ---------------------------------------------------------------------------
// Message

/** Writes, into the active logger's current chunk:
 *   - the site's SiteDefinition record, iff site.announced_ is 0 (relaxed
 *     load; set to 1 after the definition commits) -- the definition always
 *     precedes the site's first message in the stream;
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

    if (site.announced_.load(std::memory_order_relaxed) == 0u) {
        writeSiteDefinition<Args...>(*logger, site);
        site.announced_.store(1u, std::memory_order_relaxed);
    }

    const std::uint32_t argsBytes = upperBoundSize(args...);
    const std::uint32_t payloadBytes =
        static_cast<std::uint32_t>(sizeof(wire::MessagePayload)) + argsBytes;

    ChunkWriter* writer = logger->currentWriter();
    ChunkWriter::Reservation slot =
        (writer != nullptr) ? writer->reserve(payloadBytes) : ChunkWriter::Reservation{};

    if (!slot.valid()) {
        writer = logger->refillWriter();
        slot = (writer != nullptr) ? writer->reserve(payloadBytes) : ChunkWriter::Reservation{};
        if (!slot.valid()) {
            logger->countDrop();
            return;
        }
    }

    const wire::MessagePayload messagePayload{site.id(), monotonicNowNs(), currentCorrelation()};
    wire::storeUnaligned(slot.payload_, messagePayload);

    const EncodeResult encoded =
        encodeArgs(slot.payload_ + sizeof(wire::MessagePayload),
                  payloadBytes - static_cast<std::uint32_t>(sizeof(wire::MessagePayload)), args...);

    wire::RecordHead head{};
    head.payloadBytes_ =
        static_cast<std::uint16_t>(sizeof(wire::MessagePayload) + encoded.bytes_);
    head.kind_ = wire::RecordKind::Message;
    head.flags_ = encoded.truncated_ ? wire::cFlagTruncated : std::uint8_t{0};

    writer->commit(slot, head);

    if (encoded.truncated_) {
        logger->countTruncation();
    }
}

} // namespace sub0log::detail
