// The format contract itself: head-word packing, alignment arithmetic and
// the pinned struct sizes. If any of this moves, that is a format version
// bump, and this file is where the argument starts.

#include <sub0log/wire.hpp>

#include "support/test_framework.hpp"

using namespace sub0log;

TEST_CASE("record head word round-trips and carries the commit tag")
{
    const wire::RecordHead head{
        .payloadBytes_ = 123u,
        .kind_ = wire::RecordKind::Message,
        .flags_ = wire::cFlagTruncated,
        .sequence_ = 7u,
    };
    const std::uint64_t word = head.pack();

    CHECK(wire::RecordHead::isCommitted(word));
    const wire::RecordHead back = wire::RecordHead::unpack(word);
    CHECK(back.payloadBytes_ == 123u);
    CHECK(back.kind_ == wire::RecordKind::Message);
    CHECK(back.flags_ == wire::cFlagTruncated);
    CHECK(back.sequence_ == 7u);
}

TEST_CASE("a zero word and a torn word are not committed records")
{
    CHECK_FALSE(wire::RecordHead::isCommitted(0u));
    // A torn 8-byte store that only landed the low half leaves the tag zero.
    const std::uint64_t torn = wire::RecordHead{.payloadBytes_ = 8u}.pack() & 0xFFFFFFFFull;
    CHECK_FALSE(wire::RecordHead::isCommitted(torn));
}

TEST_CASE("payload padding keeps records 8-byte aligned")
{
    CHECK(wire::paddedPayload(0u) == 0u);
    CHECK(wire::paddedPayload(1u) == 8u);
    CHECK(wire::paddedPayload(8u) == 8u);
    CHECK(wire::paddedPayload(9u) == 16u);
}

TEST_CASE("on-disk struct sizes are pinned")
{
    // Compile-time in wire.hpp too; repeated here so a change shows up as a
    // named test failure rather than only a build break.
    CHECK(sizeof(wire::SegmentHeader) == 64u);
    CHECK(sizeof(wire::ChunkHeader) == 32u);
    CHECK(sizeof(wire::MessagePayload) == 24u);
    CHECK(sizeof(wire::SiteDefinitionPayload) == 24u);
    CHECK(sizeof(wire::ChildStartPayload) == 32u);
    CHECK(sizeof(wire::ChildOutputPayload) == 24u);
    CHECK(sizeof(wire::ChildExitPayload) == 24u);
}

TEST_CASE("chunk size classes round-trip and cover every size this codebase actually uses")
{
    // Every chunkBytes_ value configured anywhere in this repository, before
    // and after this field existed (grep the tree if this list ever looks
    // suspicious) -- proof the quantisation this field imposes never
    // actually restricted a real configuration.
    static constexpr std::uint32_t cSizesInUse[] = {
        128u, 256u, 1024u, 4096u, 16384u, wire::cDefaultChunkBytes,
    };
    for (const std::uint32_t bytes : cSizesInUse) {
        const auto sizeClass = wire::sizeClassForChunkBytes(bytes);
        REQUIRE(sizeClass.has_value());
        CHECK(wire::chunkBytesForSizeClass(*sizeClass) == bytes);
    }
}

TEST_CASE("an unrepresentable chunk size has no class, not a wrong one")
{
    CHECK_FALSE(wire::sizeClassForChunkBytes(1000u).has_value());
    CHECK_FALSE(wire::sizeClassForChunkBytes(0u).has_value());
    CHECK_FALSE(wire::sizeClassForChunkBytes(100u).has_value()); // below cChunkSizeUnit entirely
}

TEST_CASE("the largest size class is exactly one default segment")
{
    // cMaxChunkSizeClass's own justification (wire.hpp): a chunk configured
    // larger than the whole default segment leaves room for at most one.
    CHECK(wire::chunkBytesForSizeClass(wire::cMaxChunkSizeClass) == wire::cDefaultSegmentBytes);
}

TEST_CASE("ChunkHeader's size-class field defaults to the unspecified sentinel")
{
    // Aggregate-initialising without it -- exactly how every call site that
    // predates this field still reads (tests/support/segment_image.hpp's
    // stampChunk(), Segment::claimChunk() before this change) -- must still
    // mean "unspecified", not a real 128-byte class, or every segment ever
    // written before this field existed would misdecode under a reader that
    // trusted it (wire.hpp's cChunkSizeUnit comment is the argument this
    // pins down as code).
    const wire::ChunkHeader header{1u, 2u, 3u};
    CHECK(header.chunkSizeClass_ == 0u);
}

TEST_CASE("ChunkHeader's checksum field defaults to the not-present sentinel")
{
    // Same reasoning, one field over (docs/vnext-header-checksum.md): a
    // segment written before headerChecksum_ existed left this byte as
    // silent, always-zero reserved space, and a reader must keep reading
    // that as "not present" rather than as a checksum that happens to be 0.
    const wire::ChunkHeader header{1u, 2u, 3u};
    CHECK(header.headerChecksum_ == 0u);
}

TEST_CASE("crc32 matches the known test vector for \"123456789\"")
{
    // The standard CRC-32/ISO-HDLC check value, quoted by every
    // implementation of this exact polynomial (0xEDB88320) -- if this ever
    // fails, the algorithm changed, not just this codebase's use of it.
    const auto* const bytes = reinterpret_cast<const std::byte*>("123456789");
    CHECK(wire::crc32(0u, bytes, 9u) == 0xCBF43926u);
}

TEST_CASE("crc32 is chainable: one call over N bytes equals two calls over any split")
{
    const std::uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    const auto* const bytes = reinterpret_cast<const std::byte*>(data);
    const std::uint32_t whole = wire::crc32(0u, bytes, 6u);
    for (std::size_t split = 0u; split <= 6u; ++split) {
        const std::uint32_t chained =
            wire::crc32(wire::crc32(0u, bytes, split), bytes + split, 6u - split);
        CHECK(chained == whole);
    }
}

TEST_CASE("computeChunkHeaderChecksum changes if any checksummed field changes")
{
    const std::uint32_t base = wire::computeChunkHeaderChecksum(1u, 2u, 3u, 4u);
    CHECK(wire::computeChunkHeaderChecksum(9u, 2u, 3u, 4u) != base); // generation
    CHECK(wire::computeChunkHeaderChecksum(1u, 9u, 3u, 4u) != base); // ownerThread
    CHECK(wire::computeChunkHeaderChecksum(1u, 2u, 9u, 4u) != base); // claimMonoNs
    CHECK(wire::computeChunkHeaderChecksum(1u, 2u, 3u, 9u) != base); // chunkSizeClass
}

TEST_CASE("computeChunkHeaderChecksum never returns the 0 sentinel")
{
    // 0 is reserved to mean "not present" (never-claimed chunk, or one
    // stamped before this field existed); a real computation that would
    // otherwise land on exactly 0 is remapped to 1, so a reader can treat
    // stored 0 as unambiguous. This sweeps varied inputs, including the
    // all-zero case (0, 0, 0, 0) -- a genuinely stamped chunk with a zero
    // thread id and zero timestamp is not something the format can rule
    // out, so it is exactly the input where "real checksum" and "sentinel"
    // would collide if nothing remapped them.
    CHECK(wire::computeChunkHeaderChecksum(0u, 0u, 0u, 0u) != 0u);
    for (std::uint64_t generation = 1u; generation < 500u; ++generation) {
        CHECK(wire::computeChunkHeaderChecksum(generation, generation * 7u, generation * 13u,
                                               static_cast<std::uint8_t>(generation)) != 0u);
    }
}
