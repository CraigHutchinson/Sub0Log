// Merger: N-way merge of decoded segments into one ordered stream (R5.2),
// aligned across processes whose monotonic epochs differ (R5.3). Segment
// images are hand-built, independent of the producer (see reader.test.cpp).

#include <sub0log/merge.hpp>

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

using namespace sub0log;

namespace {

template <typename T>
void appendRaw(std::vector<std::byte>& buf, const T value)
{
    const std::size_t offset = buf.size();
    buf.resize(offset + sizeof(T));
    wire::storeUnaligned(buf.data() + offset, value);
}

void appendBytesRaw(std::vector<std::byte>& buf, const std::string_view bytes)
{
    const std::size_t offset = buf.size();
    buf.resize(offset + bytes.size());
    std::memcpy(buf.data() + offset, bytes.data(), bytes.size());
}

void appendLengthPrefixed(std::vector<std::byte>& buf, const std::string_view s)
{
    appendRaw<std::uint16_t>(buf, static_cast<std::uint16_t>(s.size()));
    appendBytesRaw(buf, s);
}

std::vector<std::byte> buildSiteDefinitionPayload(const std::uint64_t siteId,
                                                    const std::string_view format)
{
    std::vector<std::byte> buf;
    wire::SiteDefinitionPayload prefix{};
    prefix.siteId_ = siteId;
    prefix.subsystemId_ = 0u;
    prefix.line_ = 1u;
    prefix.severity_ = static_cast<std::uint8_t>(Severity::Info);
    prefix.argCount_ = 0u;
    appendRaw(buf, prefix);
    appendLengthPrefixed(buf, format); // no arg types to append (argCount_ == 0)
    appendLengthPrefixed(buf, "test.cpp");
    return buf;
}

class SegmentImageBuilder {
public:
    SegmentImageBuilder(const std::uint32_t chunkBytes, const std::uint32_t chunkCount,
                         const std::uint64_t generation, const std::uint64_t processId,
                         const std::uint64_t anchorMonoNs, const std::uint64_t anchorWallNs)
        : chunkBytes_(chunkBytes), generation_(generation)
    {
        const std::uint64_t segmentBytes =
            wire::cSegmentHeaderBytes + static_cast<std::uint64_t>(chunkCount) * chunkBytes;
        image_.assign(segmentBytes, std::byte{0});

        wire::SegmentHeader h{};
        h.magic_ = wire::cMagic;
        h.formatVersion_ = wire::cFormatVersion;
        h.headerBytes_ = wire::cSegmentHeaderBytes;
        h.chunkBytes_ = chunkBytes;
        h.segmentBytes_ = segmentBytes;
        h.generation_ = generation;
        h.processId_ = processId;
        h.anchorMonoNs_ = anchorMonoNs;
        h.anchorWallNs_ = anchorWallNs;
        wire::storeUnaligned(image_.data(), h);
    }

    [[nodiscard]] std::uint64_t chunkOffset(const std::uint32_t index) const noexcept
    {
        return wire::cSegmentHeaderBytes + static_cast<std::uint64_t>(index) * chunkBytes_;
    }

    std::uint64_t stampChunk(const std::uint32_t index, const std::uint64_t ownerThread,
                              const std::uint64_t generationOverride = 0)
    {
        const std::uint64_t offset = chunkOffset(index);
        const std::uint64_t gen = generationOverride != 0 ? generationOverride : generation_;
        wire::ChunkHeader ch{gen, ownerThread, 0, 0};
        wire::storeUnaligned(image_.data() + offset, ch);
        return offset + sizeof(wire::ChunkHeader);
    }

    std::uint64_t writeRecord(const std::uint64_t cursor, const wire::RecordKind kind,
                               const std::vector<std::byte>& payload)
    {
        const auto payloadBytes = static_cast<std::uint16_t>(payload.size());
        if (!payload.empty()) {
            std::memcpy(image_.data() + cursor + 8, payload.data(), payload.size());
        }
        const wire::RecordHead head{payloadBytes, kind, 0, 0};
        wire::storeUnaligned(image_.data() + cursor, head.pack());
        return cursor + 8 + wire::paddedPayload(payloadBytes);
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept { return image_; }

private:
    std::vector<std::byte> image_;
    std::uint32_t chunkBytes_;
    std::uint64_t generation_;
};

/// One site definition followed by one message at `monoNs`, on the given
/// owning thread. Returns the image (kept alive by the caller).
SegmentImageBuilder oneRecordSegment(const std::uint64_t processId,
                                      const std::uint64_t anchorMonoNs,
                                      const std::uint64_t anchorWallNs,
                                      const std::uint64_t siteId, const std::uint64_t monoNs,
                                      const std::uint64_t ownerThread,
                                      const std::uint64_t generation = 1)
{
    SegmentImageBuilder builder(1024u, 1u, generation, processId, anchorMonoNs, anchorWallNs);
    std::uint64_t cursor = builder.stampChunk(0, ownerThread);
    cursor = builder.writeRecord(cursor, wire::RecordKind::SiteDefinition,
                                  buildSiteDefinitionPayload(siteId, "e"));
    wire::MessagePayload msg{};
    msg.siteId_ = siteId;
    msg.monoNs_ = monoNs;
    msg.correlationId_ = 0u;
    std::vector<std::byte> payload;
    appendRaw(payload, msg);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, payload);
    (void)cursor;
    return builder;
}

} // namespace

// ---------------------------------------------------------------------------
// Two segments, same events, shifted epochs: alignment must interleave them
// by real time, not by raw monoNs (which is only comparable within a
// process's own epoch until anchored, R5.3).

TEST_CASE("segments with different anchor pairs merge into aligned global order")
{
    // Segment A: a "normal" small epoch.
    SegmentImageBuilder segA(1024u, 1u, /*generation=*/10u, /*processId=*/1u,
                              /*anchorMonoNs=*/1000u, /*anchorWallNs=*/1'000'000'000ull);
    {
        std::uint64_t cursor = segA.stampChunk(0, /*ownerThread=*/1u);
        cursor = segA.writeRecord(cursor, wire::RecordKind::SiteDefinition,
                                   buildSiteDefinitionPayload(1u, "a1"));
        wire::MessagePayload m1{};
        m1.siteId_ = 1u;
        m1.monoNs_ = 1000u; // aligned: 1e9 + 0
        std::vector<std::byte> p1;
        appendRaw(p1, m1);
        cursor = segA.writeRecord(cursor, wire::RecordKind::Message, p1);

        wire::MessagePayload m2{};
        m2.siteId_ = 1u;
        m2.monoNs_ = 2000u; // aligned: 1e9 + 1000
        std::vector<std::byte> p2;
        appendRaw(p2, m2);
        cursor = segA.writeRecord(cursor, wire::RecordKind::Message, p2);
        (void)cursor;
    }

    // Segment B: a different process, whose monotonic reading has a huge,
    // unrelated numeric offset from A's (the whole reason steady_clock
    // epochs are not portably comparable across processes), but whose
    // anchor places it a few hundred nanoseconds after A's anchor in wall
    // time.
    SegmentImageBuilder segB(1024u, 1u, /*generation=*/20u, /*processId=*/2u,
                              /*anchorMonoNs=*/500000u, /*anchorWallNs=*/1'000'000'500ull);
    {
        std::uint64_t cursor = segB.stampChunk(0, /*ownerThread=*/9u);
        cursor = segB.writeRecord(cursor, wire::RecordKind::SiteDefinition,
                                   buildSiteDefinitionPayload(2u, "b1"));
        wire::MessagePayload m1{};
        m1.siteId_ = 2u;
        m1.monoNs_ = 500000u; // aligned: 1e9 + 500
        std::vector<std::byte> p1;
        appendRaw(p1, m1);
        cursor = segB.writeRecord(cursor, wire::RecordKind::Message, p1);

        wire::MessagePayload m2{};
        m2.siteId_ = 2u;
        m2.monoNs_ = 501000u; // aligned: 1e9 + 1500
        std::vector<std::byte> p2;
        appendRaw(p2, m2);
        cursor = segB.writeRecord(cursor, wire::RecordKind::Message, p2);
        (void)cursor;
    }

    Merger merger;
    REQUIRE(merger.addSegment(segA.span()) == SegmentError::Ok);
    REQUIRE(merger.addSegment(segB.span()) == SegmentError::Ok);

    const std::vector<MergedRecord> merged = merger.merged();
    REQUIRE(merged.size() == 4u);

    // Expected global order by aligned wall time:
    //   A@1e9+0, B@1e9+500, A@1e9+1000, B@1e9+1500
    CHECK(merged[0].processId_ == 1u);
    CHECK(merged[0].alignedNs_ == 1'000'000'000ull);
    CHECK(merged[1].processId_ == 2u);
    CHECK(merged[1].alignedNs_ == 1'000'000'500ull);
    CHECK(merged[2].processId_ == 1u);
    CHECK(merged[2].alignedNs_ == 1'000'001'000ull);
    CHECK(merged[3].processId_ == 2u);
    CHECK(merged[3].alignedNs_ == 1'000'001'500ull);

    for (const MergedRecord& r : merged) {
        REQUIRE(r.record_.site_ != nullptr);
    }
    CHECK(merger.totals().unreadableBytes_ > 0u); // trailing chunk space, both segments
    CHECK(merger.totals().undecodableRecords_ == 0u);
}

// ---------------------------------------------------------------------------
// Deterministic tie-breaks: (processId_, ownerThread_, monoNs_), in order.

TEST_CASE("ties at equal alignedNs break by processId, then ownerThread, then monoNs")
{
    Merger merger;

    // Case A: equal alignedNs, different processId -> lower processId first.
    SegmentImageBuilder segHighPid = oneRecordSegment(/*processId=*/5u, 0u, 100u, 1u, 0u, 1u, 1u);
    SegmentImageBuilder segLowPid = oneRecordSegment(/*processId=*/3u, 0u, 100u, 1u, 0u, 1u, 2u);
    REQUIRE(merger.addSegment(segHighPid.span()) == SegmentError::Ok);
    REQUIRE(merger.addSegment(segLowPid.span()) == SegmentError::Ok);

    // Case B: equal alignedNs, equal processId, different ownerThread ->
    // lower ownerThread first. Two chunks in one segment, same monoNs.
    SegmentImageBuilder segThreads(1024u, 2u, /*generation=*/30u, /*processId=*/9u, 0u, 200u);
    {
        std::uint64_t c0 = segThreads.stampChunk(0, /*ownerThread=*/50u);
        c0 = segThreads.writeRecord(c0, wire::RecordKind::SiteDefinition,
                                     buildSiteDefinitionPayload(7u, "t"));
        wire::MessagePayload m0{};
        m0.siteId_ = 7u;
        m0.monoNs_ = 0u;
        std::vector<std::byte> p0;
        appendRaw(p0, m0);
        c0 = segThreads.writeRecord(c0, wire::RecordKind::Message, p0);
        (void)c0;

        std::uint64_t c1 = segThreads.stampChunk(1, /*ownerThread=*/20u);
        c1 = segThreads.writeRecord(c1, wire::RecordKind::SiteDefinition,
                                     buildSiteDefinitionPayload(8u, "t2"));
        wire::MessagePayload m1{};
        m1.siteId_ = 8u;
        m1.monoNs_ = 0u;
        std::vector<std::byte> p1;
        appendRaw(p1, m1);
        c1 = segThreads.writeRecord(c1, wire::RecordKind::Message, p1);
        (void)c1;
    }
    REQUIRE(merger.addSegment(segThreads.span()) == SegmentError::Ok);

    // Case C: equal alignedNs, equal processId, equal ownerThread, different
    // monoNs (only possible from two segments with different anchors) ->
    // lower monoNs first.
    SegmentImageBuilder segMonoLow =
        oneRecordSegment(/*processId=*/7u, /*anchorMonoNs=*/0u, /*anchorWallNs=*/1000u,
                          /*siteId=*/9u, /*monoNs=*/50u, /*ownerThread=*/1u, /*generation=*/4u);
    SegmentImageBuilder segMonoHigh =
        oneRecordSegment(/*processId=*/7u, /*anchorMonoNs=*/1000u, /*anchorWallNs=*/0u,
                          /*siteId=*/9u, /*monoNs=*/2050u, /*ownerThread=*/1u, /*generation=*/5u);
    // aligned(low)  = 1000 + (50-0)     = 1050
    // aligned(high) = 0    + (2050-1000) = 1050
    REQUIRE(merger.addSegment(segMonoHigh.span()) == SegmentError::Ok); // added out of order
    REQUIRE(merger.addSegment(segMonoLow.span()) == SegmentError::Ok);

    const std::vector<MergedRecord> merged = merger.merged();
    REQUIRE(merged.size() == 6u);

    // Case A pair: alignedNs 100 for both, processId 3 before 5.
    CHECK(merged[0].alignedNs_ == 100u);
    CHECK(merged[0].processId_ == 3u);
    CHECK(merged[1].alignedNs_ == 100u);
    CHECK(merged[1].processId_ == 5u);

    // Case B pair: alignedNs 200 for both, ownerThread 20 before 50.
    CHECK(merged[2].alignedNs_ == 200u);
    CHECK(merged[2].record_.ownerThread_ == 20u);
    CHECK(merged[3].alignedNs_ == 200u);
    CHECK(merged[3].record_.ownerThread_ == 50u);

    // Case C pair: alignedNs 1050 for both, monoNs 50 before 2050.
    CHECK(merged[4].alignedNs_ == 1050u);
    CHECK(merged[4].record_.monoNs_ == 50u);
    CHECK(merged[5].alignedNs_ == 1050u);
    CHECK(merged[5].record_.monoNs_ == 2050u);
}

// ---------------------------------------------------------------------------
// Totals accumulate across segments.

TEST_CASE("Merger totals sum unreadable bytes and undecodable records across segments")
{
    // Segment 1: a stale chunk contributes unreadable bytes.
    SegmentImageBuilder seg1(256u, 2u, /*generation=*/40u, 1u, 0u, 0u);
    {
        std::uint64_t c0 = seg1.stampChunk(0, 1u);
        c0 = seg1.writeRecord(c0, wire::RecordKind::SiteDefinition,
                               buildSiteDefinitionPayload(1u, "s"));
        wire::MessagePayload m{};
        m.siteId_ = 1u;
        std::vector<std::byte> p;
        appendRaw(p, m);
        c0 = seg1.writeRecord(c0, wire::RecordKind::Message, p);
        (void)c0;
        seg1.stampChunk(1, 2u, /*generationOverride=*/999u); // stale: wrong generation
    }

    // Segment 2: a message with a missing site definition.
    SegmentImageBuilder seg2(256u, 1u, /*generation=*/50u, 2u, 0u, 0u);
    {
        std::uint64_t c0 = seg2.stampChunk(0, 1u);
        wire::MessagePayload m{};
        m.siteId_ = 0xFFFFu; // never defined in this segment
        std::vector<std::byte> p;
        appendRaw(p, m);
        c0 = seg2.writeRecord(c0, wire::RecordKind::Message, p);
        (void)c0;
    }

    Merger merger;
    REQUIRE(merger.addSegment(seg1.span()) == SegmentError::Ok);
    REQUIRE(merger.addSegment(seg2.span()) == SegmentError::Ok);

    const Merger::Totals totals = merger.totals();
    // seg1's stale chunk body (256 - sizeof(ChunkHeader)) plus its trailing
    // unwritten space in chunk 0 must both be present.
    CHECK(totals.unreadableBytes_ >= (256u - sizeof(wire::ChunkHeader)));
    CHECK(totals.undecodableRecords_ == 1u);

    const std::vector<MergedRecord> merged = merger.merged();
    REQUIRE(merged.size() == 1u); // only seg1's message decoded
    CHECK(merged[0].processId_ == 1u);
}
