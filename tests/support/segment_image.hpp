#pragma once

/** @file tests/support/segment_image.hpp
 *  @brief Hand-builds segment images byte by byte from wire.hpp primitives.
 *
 *  This is the reader-side test double: a fake segment with none of the
 *  producer involved, so reader tests stay independent of the code under
 *  test on the other side of the file boundary (docs/architecture.md: "a
 *  reader never touches producer state"). Damage -- torn words, stale
 *  generations, hostile lengths -- is injected here byte-precisely.
 */

#include <sub0log/severity.hpp>
#include <sub0log/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace sub0log::test {

// ---------------------------------------------------------------------------
// Byte-level helpers: append a value's wire representation to a buffer.

template <typename T>
inline void appendRaw(std::vector<std::byte>& buf, const T value)
{
    const std::size_t offset = buf.size();
    buf.resize(offset + sizeof(T));
    wire::storeUnaligned(buf.data() + offset, value);
}

inline void appendBytesRaw(std::vector<std::byte>& buf, const std::string_view bytes)
{
    const std::size_t offset = buf.size();
    buf.resize(offset + bytes.size());
    std::memcpy(buf.data() + offset, bytes.data(), bytes.size());
}

/// u16 length + bytes, as SiteDefinition's format/file tail and a Bytes arg.
inline void appendLengthPrefixed(std::vector<std::byte>& buf, const std::string_view s)
{
    appendRaw<std::uint16_t>(buf, static_cast<std::uint16_t>(s.size()));
    appendBytesRaw(buf, s);
}

inline std::vector<std::byte> buildSiteDefinitionPayload(
    const std::uint64_t siteId, const std::uint32_t subsystemId, const std::uint32_t line,
    const Severity severity, const std::vector<wire::TypeCode>& argTypes,
    const std::string_view format, const std::string_view file)
{
    std::vector<std::byte> buf;
    wire::SiteDefinitionPayload prefix{};
    prefix.siteId_ = siteId;
    prefix.subsystemId_ = subsystemId;
    prefix.line_ = line;
    prefix.severity_ = static_cast<std::uint8_t>(severity);
    prefix.argCount_ = static_cast<std::uint8_t>(argTypes.size());
    appendRaw(buf, prefix);
    for (const wire::TypeCode t : argTypes) {
        appendRaw<std::uint8_t>(buf, static_cast<std::uint8_t>(t));
    }
    appendLengthPrefixed(buf, format);
    appendLengthPrefixed(buf, file);
    return buf;
}

/// The zero-argument convenience most merge tests want: only identity and a
/// format string vary.
inline std::vector<std::byte> buildSiteDefinitionPayload(const std::uint64_t siteId,
                                                         const std::string_view format)
{
    return buildSiteDefinitionPayload(siteId, 0u, 1u, Severity::Info, {}, format, "test.cpp");
}

// ---------------------------------------------------------------------------
// SegmentImageBuilder: assembles a valid segment image in memory.

class SegmentImageBuilder {
public:
    SegmentImageBuilder(const std::uint32_t headerBytes, const std::uint32_t chunkBytes,
                        const std::uint32_t chunkCount, const std::uint64_t generation,
                        const std::uint64_t processId, const std::uint64_t anchorMonoNs,
                        const std::uint64_t anchorWallNs)
        : headerBytes_(headerBytes), chunkBytes_(chunkBytes), generation_(generation)
    {
        const std::uint64_t segmentBytes =
            headerBytes + static_cast<std::uint64_t>(chunkCount) * chunkBytes;
        image_.assign(segmentBytes, std::byte{0});

        wire::SegmentHeader h{};
        h.magic_ = wire::cMagic;
        h.formatVersion_ = wire::cFormatVersion;
        h.headerBytes_ = headerBytes;
        h.chunkBytes_ = chunkBytes;
        h.segmentBytes_ = segmentBytes;
        h.generation_ = generation;
        h.processId_ = processId;
        h.anchorMonoNs_ = anchorMonoNs;
        h.anchorWallNs_ = anchorWallNs;
        wire::storeUnaligned(image_.data(), h);
    }

    /// The common case: a standard-size header page.
    SegmentImageBuilder(const std::uint32_t chunkBytes, const std::uint32_t chunkCount,
                        const std::uint64_t generation, const std::uint64_t processId,
                        const std::uint64_t anchorMonoNs, const std::uint64_t anchorWallNs)
        : SegmentImageBuilder(wire::cSegmentHeaderBytes, chunkBytes, chunkCount, generation,
                              processId, anchorMonoNs, anchorWallNs)
    {
    }

    [[nodiscard]] std::uint32_t chunkBytes() const noexcept { return chunkBytes_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] std::uint64_t chunkOffset(const std::uint32_t index) const noexcept
    {
        return headerBytes_ + static_cast<std::uint64_t>(index) * chunkBytes_;
    }

    /// Stamps a chunk's header with an explicit generation (pass something
    /// other than generation() to fake a stale chunk); returns the offset of
    /// the chunk's record body. headerChecksum_ is left at its default-
    /// member-initialised 0 ("not present") -- field-by-field assignment,
    /// not positional aggregate-init braces, so the fields this helper does
    /// not mention (headerChecksum_, chunkSizeClass_) always mean their
    /// documented "not present"/"unspecified" default regardless of where
    /// ChunkHeader gains its next field.
    std::uint64_t stampChunk(const std::uint32_t index, const std::uint64_t generation,
                             const std::uint64_t ownerThread, const std::uint64_t claimMonoNs = 0)
    {
        const std::uint64_t offset = chunkOffset(index);
        wire::ChunkHeader ch{};
        ch.generation_ = generation;
        ch.ownerThread_ = ownerThread;
        ch.claimMonoNs_ = claimMonoNs;
        wire::storeUnaligned(image_.data() + offset, ch);
        return offset + sizeof(wire::ChunkHeader);
    }

    /// Stamps a chunk that belongs to this segment's own generation.
    std::uint64_t stampOwnedChunk(const std::uint32_t index, const std::uint64_t ownerThread)
    {
        return stampChunk(index, generation_, ownerThread);
    }

    /// Stamps an owned chunk with an explicit wire::ChunkHeader::
    /// chunkSizeClass_ instead of the 0 ("unspecified") every method above
    /// uses -- for tests that exercise the self-description cross-check
    /// itself, agreement and a deliberately wrong class alike.
    /// headerChecksum_ stays 0 ("not present"), same reasoning as
    /// stampChunk(): a test built through this helper is exercising size-
    /// class behaviour, not checksum behaviour, and 0 skips verification
    /// exactly as legacy data does.
    std::uint64_t stampOwnedChunkWithSizeClass(const std::uint32_t index,
                                               const std::uint64_t ownerThread,
                                               const std::uint8_t chunkSizeClass)
    {
        const std::uint64_t offset = chunkOffset(index);
        wire::ChunkHeader ch{};
        ch.generation_ = generation_;
        ch.ownerThread_ = ownerThread;
        ch.chunkSizeClass_ = chunkSizeClass;
        wire::storeUnaligned(image_.data() + offset, ch);
        return offset + sizeof(wire::ChunkHeader);
    }

    /// Stamps an owned chunk with a real, matching headerChecksum_ -- the
    /// same value Segment::claimChunk() would compute for these fields --
    /// so a test can exercise the checksum-verified path itself. A
    /// corrupted-checksum test starts from this and then calls
    /// corruptByte() below, rather than hand-computing a wrong checksum:
    /// the point is that *any* of the checksummed fields being tampered is
    /// caught, not just the checksum field itself.
    std::uint64_t stampOwnedChunkWithChecksum(const std::uint32_t index,
                                              const std::uint64_t ownerThread,
                                              const std::uint64_t claimMonoNs,
                                              const std::uint8_t chunkSizeClass)
    {
        const std::uint64_t offset = chunkOffset(index);
        wire::ChunkHeader ch{};
        ch.generation_ = generation_;
        ch.ownerThread_ = ownerThread;
        ch.claimMonoNs_ = claimMonoNs;
        ch.chunkSizeClass_ = chunkSizeClass;
        ch.headerChecksum_ = wire::computeChunkHeaderChecksum(
            ch.generation_, ch.ownerThread_, ch.claimMonoNs_, ch.chunkSizeClass_);
        wire::storeUnaligned(image_.data() + offset, ch);
        return offset + sizeof(wire::ChunkHeader);
    }

    /// Overwrites one byte anywhere in the image directly -- for tests that
    /// need to corrupt a specific field (by offsetof(wire::ChunkHeader, ...)
    /// added to chunkOffset()) after stamping an otherwise-valid header.
    void corruptByte(const std::uint64_t offset, const std::uint8_t newByte)
    {
        image_[offset] = static_cast<std::byte>(newByte);
    }

    /// Writes one committed record at `cursor` (payload before the head
    /// word -- commit-last). Returns the next cursor.
    std::uint64_t writeRecord(const std::uint64_t cursor, const wire::RecordKind kind,
                              const std::uint8_t flags, const std::uint16_t sequence,
                              const std::vector<std::byte>& payload)
    {
        const auto payloadBytes = static_cast<std::uint16_t>(payload.size());
        if (!payload.empty()) {
            std::memcpy(image_.data() + cursor + 8, payload.data(), payload.size());
        }
        const wire::RecordHead head{payloadBytes, kind, flags, sequence};
        wire::storeUnaligned(image_.data() + cursor, head.pack());
        return cursor + 8 + wire::paddedPayload(payloadBytes);
    }

    /// Flagless, sequence-zero convenience.
    std::uint64_t writeRecord(const std::uint64_t cursor, const wire::RecordKind kind,
                              const std::vector<std::byte>& payload)
    {
        return writeRecord(cursor, kind, 0u, 0u, payload);
    }

    /// Writes a raw 8-byte word directly (for torn/garbage head-word tests).
    void writeRawWord(const std::uint64_t cursor, const std::uint64_t word)
    {
        wire::storeUnaligned(image_.data() + cursor, word);
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept { return image_; }
    [[nodiscard]] std::span<std::byte> mutableSpan() noexcept { return image_; }

private:
    std::vector<std::byte> image_;
    std::uint32_t headerBytes_;
    std::uint32_t chunkBytes_;
    std::uint64_t generation_;
};

} // namespace sub0log::test
