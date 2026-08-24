// The format contract itself: head-word packing, alignment arithmetic and
// the pinned struct sizes. If any of this moves, that is a format version
// bump, and this file is where the argument starts.

#include <sub0log/wire.hpp>

#include <doctest/doctest.h>

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
}
