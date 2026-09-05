// SegmentReader and Decoder: raw record recovery and typed decoding.
// Segment images are built BY HAND from wire.hpp primitives -- deliberately
// independent of the producer, per the reader's contract (docs/architecture.md:
// "a reader never touches producer state").

#include <sub0log/reader.hpp>

#include "support/segment_image.hpp"

#include "support/test_framework.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace sub0log;
using namespace sub0log::test;



// ---------------------------------------------------------------------------
// (1) Happy path: definitions + messages with several arg types round-trip.

TEST_CASE("decoder round-trips typed arguments through a hand-built segment")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 1024u, 1u, /*generation=*/12345u,
                                 /*processId=*/999u, /*anchorMonoNs=*/1000u,
                                 /*anchorWallNs=*/2'000'000'000'000ull);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), /*ownerThread=*/42u);

    const std::vector<wire::TypeCode> argTypes{
        wire::TypeCode::I32, wire::TypeCode::U64, wire::TypeCode::F64,
        wire::TypeCode::Bool, wire::TypeCode::Bytes,
    };
    const auto defPayload = buildSiteDefinitionPayload(
        /*siteId=*/0xAAAAu, /*subsystemId=*/7u, /*line=*/100u, Severity::Info, argTypes,
        "x={} y={} z={} b={} s={}", "test.cpp");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SiteDefinition, 0, 0, defPayload);

    std::vector<std::byte> msgPayload;
    wire::MessagePayload msgPrefix{};
    msgPrefix.siteId_ = 0xAAAAu;
    msgPrefix.monoNs_ = 555u;
    msgPrefix.correlationId_ = 777u;
    appendRaw(msgPayload, msgPrefix);
    appendRaw<std::int32_t>(msgPayload, -42);
    appendRaw<std::uint64_t>(msgPayload, 123456789012345ull);
    appendRaw<double>(msgPayload, 3.14159);
    appendRaw<std::uint8_t>(msgPayload, 1u); // Bool
    appendLengthPrefixed(msgPayload, "hello");
    const std::uint64_t recordStart = cursor;
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 1, msgPayload);
    const std::uint64_t consumed = cursor - (builder.chunkOffset(0) + sizeof(wire::ChunkHeader));

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());
    CHECK(reader.error() == SegmentError::Ok);

    Decoder decoder;
    const std::vector<DecodedRecord> records = decoder.decodeAll(reader);

    REQUIRE(records.size() == 1u);
    CHECK(decoder.undecodableRecords() == 0u);

    const DecodedRecord& rec = records[0];
    REQUIRE(rec.site_ != nullptr);
    CHECK(rec.site_->siteId_ == 0xAAAAu);
    CHECK(rec.site_->subsystem_.value_ == 7u);
    CHECK(rec.site_->severity_ == Severity::Info);
    CHECK(rec.site_->line_ == 100u);
    CHECK(rec.site_->format_ == "x={} y={} z={} b={} s={}");
    CHECK(rec.site_->file_ == "test.cpp");
    CHECK(rec.monoNs_ == 555u);
    CHECK(rec.correlationId_ == 777u);
    CHECK(rec.ownerThread_ == 42u);
    CHECK_FALSE(rec.truncated_);

    REQUIRE(rec.args_.size() == 5u);
    CHECK(std::get<std::int64_t>(rec.args_[0]) == -42);
    CHECK(std::get<std::uint64_t>(rec.args_[1]) == 123456789012345ull);
    CHECK(std::get<double>(rec.args_[2]) == doctest::Approx(3.14159));
    CHECK(std::get<bool>(rec.args_[3]) == true);
    CHECK(std::get<std::string_view>(rec.args_[4]) == "hello");

    // Trailing chunk space beyond the last record was never written. It is
    // reported, not silently ignored (R3.3/R9.2), but as unwritten space --
    // this segment suffered no damage and its damage count says so.
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(reader.unwrittenBytes() == bodyBytes - consumed);
    CHECK(reader.unreadableBytes() == 0u);
    CHECK(reader.accountedBytes() == bodyBytes - consumed);
    (void)recordStart;
}

// ---------------------------------------------------------------------------
// (2) An uncommitted (zero) head word mid-chunk: the writer simply stopped.

TEST_CASE("a zero head word ends the chunk as unwritten space, not as damage")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 777u, 1u, 0u, 0u);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), 1u);

    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 0xDEADBEEFu);
    const std::uint64_t bodyStart = builder.chunkOffset(0) + sizeof(wire::ChunkHeader);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 0, payload);
    const std::uint64_t consumed = cursor - bodyStart;
    // Nothing further is written: the rest of the chunk stays zero-filled,
    // i.e. unwritten space (not a torn write).

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t yielded = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++yielded; });

    CHECK(yielded == 1u);
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(damage == 0u); // a producer that stopped is not a producer that broke
    CHECK(reader.unreadableBytes() == damage);
    CHECK(reader.unwrittenBytes() == bodyBytes - consumed);
}

// ---------------------------------------------------------------------------
// (3) Torn head word: garbage without the commit tag.

TEST_CASE("a torn head word is damage, and is counted apart from unwritten space")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 777u, 1u, 0u, 0u);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), 1u);
    const std::uint64_t bodyStart = builder.chunkOffset(0) + sizeof(wire::ChunkHeader);

    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 7u);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 0, payload);
    const std::uint64_t consumed = cursor - bodyStart;

    // Garbage word whose top 16 bits are not cCommitTag (0xC511).
    builder.writeRawWord(cursor, 0xDEAD'0000'0000'0001ull);

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t yielded = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++yielded; });

    CHECK(yielded == 1u);
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    // The contrast with the previous case is the point: identical layout,
    // one non-zero word, and the reader calls this one damage. That is what
    // makes the number answer "how much did I lose?".
    CHECK(damage == bodyBytes - consumed);
    CHECK(reader.unwrittenBytes() == 0u);
}

// ---------------------------------------------------------------------------
// (4) Oversized length field: reader must stay inside the chunk.

TEST_CASE("an oversized payload length is bounds-checked before use, never crashes")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 128u, 1u, 42u, 1u, 0u, 0u);
    const std::uint64_t bodyStart = builder.stampChunk(0, builder.generation(), 1u);

    // A committed head word claiming the maximum possible payload
    // (0xFFFF), in a chunk with far less room than that.
    const wire::RecordHead adversarial{0xFFFFu, wire::RecordKind::Message, 0, 0};
    builder.writeRawWord(bodyStart, adversarial.pack());

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t yielded = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++yielded; });

    CHECK(yielded == 0u); // never yielded: it would have run outside the chunk
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(damage == bodyBytes); // whole remainder from the head word onward
    CHECK(reader.unwrittenBytes() == 0u);
}

// ---------------------------------------------------------------------------
// (5) Stale chunk generation: whole chunk skipped and counted.

TEST_CASE("a chunk from a stale generation is skipped and its body counted")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 2u, /*generation=*/999u, 1u, 0u, 0u);

    // Chunk 0: current generation, one small record.
    std::uint64_t cursor0 = builder.stampChunk(0, builder.generation(), 1u);
    const std::uint64_t body0Start = builder.chunkOffset(0) + sizeof(wire::ChunkHeader);
    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 1u);
    cursor0 = builder.writeRecord(cursor0, wire::RecordKind::Message, 0, 0, payload);
    const std::uint64_t consumed0 = cursor0 - body0Start;

    // Chunk 1: stale generation, would otherwise look structurally valid.
    std::uint64_t cursor1 = builder.stampChunk(1, /*generation=*/111u, 2u);
    cursor1 = builder.writeRecord(cursor1, wire::RecordKind::Message, 0, 0, payload);
    (void)cursor1;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::vector<std::uint32_t> chunkIndices;
    const std::uint64_t damage =
        reader.visit([&](const RecordView& v) { chunkIndices.push_back(v.chunkIndex_); });

    REQUIRE(chunkIndices.size() == 1u);
    CHECK(chunkIndices[0] == 0u);

    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    // A previous generation's chunk is damage -- positive evidence of
    // storage that is not ours (R3.4). Chunk 0's unused tail is not.
    CHECK(damage == bodyBytes);
    CHECK(reader.unwrittenBytes() == bodyBytes - consumed0);
}

// ---------------------------------------------------------------------------
// (5b) Per-chunk size class (wire::ChunkHeader::chunkSizeClass_), the
// self-description this format gained alongside adaptive chunk sizing
// (docs/vnext-adaptive-chunk-sizing.md). Phase 1 of that design keeps every
// chunk in a segment the same size, exactly as before; what changes is that
// the size is now *stamped and checked*, not only assumed.

TEST_CASE("a chunk whose stamped size class agrees with the segment decodes with no extra damage")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 55u, 1u, 0u, 0u);
    const auto sizeClass = wire::sizeClassForChunkBytes(builder.chunkBytes());
    REQUIRE(sizeClass.has_value());

    std::uint64_t cursor = builder.stampOwnedChunkWithSizeClass(0, 1u, *sizeClass);
    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 1u);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 0, payload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t seen = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++seen; });

    CHECK(seen == 1u);
    CHECK(damage == 0u); // agreement costs nothing extra -- only a mismatch does.
}

TEST_CASE("the unspecified size class (0) is trusted exactly as an old segment always was")
{
    // stampChunk() (used by every reader test that predates this field)
    // always leaves this at 0 -- proof, stated directly rather than left
    // implicit in every other test file passing, that "unspecified" means
    // "behave exactly as before", not "assume 128 bytes".
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, wire::cDefaultChunkBytes, 1u, 9u, 1u, 0u, 0u);
    std::uint64_t cursor = builder.stampOwnedChunk(0, 1u); // size class 0, unchanged from before this field existed
    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 1u);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 0, payload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t seen = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++seen; });

    CHECK(seen == 1u);
    CHECK(damage == 0u);
}

TEST_CASE("a chunk stamped smaller than the segment default is trusted, not treated as damage")
{
    // Self-describing navigation (docs/vnext-adaptive-chunk-sizing.md,
    // Phase 2): a chunk's own stamped size is now the real one, so a
    // segment built with chunkBytes_=256 whose one chunk actually declares
    // 128 reads as a genuinely 128-byte chunk -- record and all -- with
    // the true remainder correctly re-synced onto as legitimately
    // unwritten space, not flagged as damage the way Phase 1's
    // cross-check (superseded by this navigation change, not kept beside
    // it) would have.
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 3u, 1u, 0u, 0u);
    const auto smallerClass = wire::sizeClassForChunkBytes(128u); // half of what this segment declares
    REQUIRE(smallerClass.has_value());

    std::uint64_t cursor = builder.stampOwnedChunkWithSizeClass(0, 1u, *smallerClass);
    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 1u);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 0, payload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t seen = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++seen; });

    CHECK(seen == 1u);
    CHECK(damage == 0u);
    // Unwritten space comes from two places, both absence rather than
    // loss: 80 bytes inside chunk 0's own 128-byte body (96 bytes of body
    // past the ChunkHeader, less the 16 the one small record consumes),
    // plus the 128 bytes beyond it that were part of the original
    // 256-byte allocation but past what this chunk actually declared --
    // this segment's own remaining, never-claimed space.
    CHECK(reader.unwrittenBytes() == 80u + 128u);
}

TEST_CASE("a stamped size class past the format's own ceiling is damage, never decoded into a stride")
{
    // Not a real class this format ever writes (Segment::claimChunk()
    // never stamps past wire::cMaxChunkSizeClass) -- corrupted or hostile,
    // and never turned into a byte count to steer navigation by, the same
    // "bounds-check a length before it moves anything" rule R3.3 already
    // applies to RecordHead::payloadBytes_.
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 7u, 1u, 0u, 0u);
    REQUIRE(wire::cMaxChunkSizeClass < 255u); // the test value below must actually be out of range
    std::uint64_t cursor =
        builder.stampOwnedChunkWithSizeClass(0, 1u, static_cast<std::uint8_t>(255u));
    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 1u);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 0, payload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    std::size_t seen = 0;
    const std::uint64_t damage = reader.visit([&](const RecordView&) { ++seen; });

    CHECK(seen == 0u);
    CHECK(damage == builder.chunkBytes() - sizeof(wire::ChunkHeader));
}

TEST_CASE("chunks of genuinely different sizes in one segment decode in order, back to back")
{
    // The actual point of self-describing navigation: two chunks that are
    // not the same size, laid out back to back exactly as a variable-size
    // allocator would place them, both decode correctly in one pass. Built
    // by hand at the byte level (a real variable-size allocator is
    // Phase 2's still-unbuilt half, docs/vnext-adaptive-chunk-sizing.md) --
    // this proves the reader side of that future allocator already works.
    const auto smallClass = wire::sizeClassForChunkBytes(128u);
    const auto bigClass = wire::sizeClassForChunkBytes(512u);
    REQUIRE(smallClass.has_value());
    REQUIRE(bigClass.has_value());

    const std::uint32_t headerBytes = wire::cSegmentHeaderBytes;
    const std::uint64_t segmentBytes = headerBytes + 128u + 512u;
    SegmentImageBuilder builder(headerBytes, /*chunkBytes=*/128u, /*chunkCount=*/1u,
                                /*generation=*/21u, 1u, 0u, 0u);
    // The builder's own constructor sizes the image from chunkBytes*chunkCount;
    // grow it to hold both chunks (128 + 512), and correct the header's
    // declared segmentBytes_ to match -- SegmentReader trusts that field as
    // the walk's own upper bound (segEnd), independent of chunkBytes_.
    std::vector<std::byte> raw(builder.span().begin(), builder.span().end());
    raw.resize(segmentBytes, std::byte{0});
    wire::SegmentHeader h = wire::loadUnaligned<wire::SegmentHeader>(raw.data());
    h.segmentBytes_ = segmentBytes;
    wire::storeUnaligned(raw.data(), h);

    std::vector<std::byte> payload;
    appendRaw<std::uint32_t>(payload, 111u);

    // Chunk A: offset headerBytes, declares 128 bytes.
    {
        wire::ChunkHeader ch{21u, 1u, 0u};
        ch.chunkSizeClass_ = *smallClass;
        wire::storeUnaligned(raw.data() + headerBytes, ch);
        const wire::RecordHead head{static_cast<std::uint16_t>(payload.size()), wire::RecordKind::Message};
        std::memcpy(raw.data() + headerBytes + sizeof(wire::ChunkHeader) + 8, payload.data(), payload.size());
        wire::storeUnaligned(raw.data() + headerBytes + sizeof(wire::ChunkHeader), head.pack());
    }
    // Chunk B: offset headerBytes + 128 (chunk A's own declared size, not
    // a fixed 256-byte stride), declares 512 bytes.
    const std::uint64_t chunkBOffset = headerBytes + 128u;
    {
        wire::ChunkHeader ch{21u, 2u, 0u};
        ch.chunkSizeClass_ = *bigClass;
        wire::storeUnaligned(raw.data() + chunkBOffset, ch);
        const wire::RecordHead head{static_cast<std::uint16_t>(payload.size()), wire::RecordKind::Message};
        std::memcpy(raw.data() + chunkBOffset + sizeof(wire::ChunkHeader) + 8, payload.data(), payload.size());
        wire::storeUnaligned(raw.data() + chunkBOffset + sizeof(wire::ChunkHeader), head.pack());
    }

    SegmentReader reader = SegmentReader::open(raw);
    REQUIRE(reader.valid());

    std::vector<std::uint64_t> owners;
    const std::uint64_t damage = reader.visit([&](const RecordView& v) { owners.push_back(v.ownerThread_); });

    CHECK(damage == 0u);
    REQUIRE(owners.size() == 2u);
    CHECK(owners[0] == 1u); // chunk A, correctly found at its own declared size
    CHECK(owners[1] == 2u); // chunk B, found starting at A's real 128 bytes, not a 256-byte guess
}

// ---------------------------------------------------------------------------
// (6) Message whose definition is missing.

TEST_CASE("a message with no matching site definition is undecodable, others unaffected")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 1024u, 1u, 1u, 1u, 0u, 0u);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), 1u);

    const auto defPayload = buildSiteDefinitionPayload(0x1111u, 0u, 1u, Severity::Debug, {},
                                                         "known site", "a.cpp");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SiteDefinition, 0, 0, defPayload);

    wire::MessagePayload knownMsg{};
    knownMsg.siteId_ = 0x1111u;
    knownMsg.monoNs_ = 1u;
    knownMsg.correlationId_ = 0u;
    std::vector<std::byte> knownPayload;
    appendRaw(knownPayload, knownMsg);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 1, knownPayload);

    wire::MessagePayload unknownMsg{};
    unknownMsg.siteId_ = 0x2222u; // never defined
    unknownMsg.monoNs_ = 2u;
    unknownMsg.correlationId_ = 0u;
    std::vector<std::byte> unknownPayload;
    appendRaw(unknownPayload, unknownMsg);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, 0, 2, unknownPayload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());
    Decoder decoder;
    const std::vector<DecodedRecord> records = decoder.decodeAll(reader);

    REQUIRE(records.size() == 1u);
    CHECK(records[0].site_->siteId_ == 0x1111u);
    CHECK(decoder.undecodableRecords() == 1u);
}

// ---------------------------------------------------------------------------
// (7) Header validation: short image, bad magic, wrong version, bad geometry.

TEST_CASE("SegmentReader::open validates size, magic, version and geometry in order")
{
    SUBCASE("an image smaller than the header page is TooSmall")
    {
        std::vector<std::byte> tiny(10, std::byte{0});
        const SegmentReader reader = SegmentReader::open(tiny);
        CHECK_FALSE(reader.valid());
        CHECK(reader.error() == SegmentError::TooSmall);
    }

    SUBCASE("a wrong magic is BadMagic")
    {
        SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 1u, 1u, 0u, 0u);
        // Corrupt the magic field (first 8 bytes) in place.
        std::span<std::byte> mut = builder.mutableSpan();
        wire::storeUnaligned<std::uint64_t>(mut.data(), 0xBAD0BAD0BAD0BAD0ull);
        const SegmentReader reader = SegmentReader::open(builder.span());
        CHECK_FALSE(reader.valid());
        CHECK(reader.error() == SegmentError::BadMagic);
    }

    SUBCASE("an unknown format version is UnknownVersion")
    {
        SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 1u, 1u, 0u, 0u);
        std::span<std::byte> mut = builder.mutableSpan();
        wire::storeUnaligned<std::uint16_t>(mut.data() + 8, static_cast<std::uint16_t>(9999u));
        const SegmentReader reader = SegmentReader::open(builder.span());
        CHECK_FALSE(reader.valid());
        CHECK(reader.error() == SegmentError::UnknownVersion);
    }

    SUBCASE("a chunkBytes too small to hold a ChunkHeader is BadGeometry")
    {
        SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 1u, 1u, 0u, 0u);
        std::span<std::byte> mut = builder.mutableSpan();
        // chunkBytes_ sits at offset 16 in SegmentHeader (magic_ 0-7,
        // formatVersion_ 8-9, reserved0_ 10-11, headerBytes_ 12-15,
        // chunkBytes_ 16-19). Shrink it below sizeof(ChunkHeader).
        wire::storeUnaligned<std::uint32_t>(mut.data() + 16, static_cast<std::uint32_t>(4u));
        const SegmentReader reader = SegmentReader::open(builder.span());
        CHECK_FALSE(reader.valid());
        CHECK(reader.error() == SegmentError::BadGeometry);
    }

    SUBCASE("a valid header is Ok")
    {
        SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 256u, 1u, 1u, 1u, 0u, 0u);
        const SegmentReader reader = SegmentReader::open(builder.span());
        CHECK(reader.valid());
        CHECK(reader.error() == SegmentError::Ok);
    }
}

// ---------------------------------------------------------------------------
// (8) format(): substitution, literal braces, surplus placeholder/args, and
//     the truncated flag surfaced from the wire record.

TEST_CASE("format() substitutes placeholders, treats doubled braces as literal, "
          "and marks a surplus placeholder")
{
    DecodedSite site;
    site.format_ = "literal open {{ and close }}, v0={}, v1={}, v2={}";

    DecodedRecord rec;
    rec.site_ = &site;
    rec.args_ = {DecodedArg{std::int64_t{5}}, DecodedArg{std::string_view{"hi"}}};
    // A third arg is deliberately absent: v2's "{}" has no matching value.

    const std::string text = Decoder::format(rec);
    CHECK(text == "literal open { and close }, v0=5, v1=hi, v2={?}");
}

TEST_CASE("format() ignores surplus arguments with no placeholder to fill")
{
    DecodedSite site;
    site.format_ = "only one slot: {}";

    DecodedRecord rec;
    rec.site_ = &site;
    rec.args_ = {DecodedArg{std::int64_t{1}}, DecodedArg{std::int64_t{2}},
                 DecodedArg{std::int64_t{3}}};

    CHECK(Decoder::format(rec) == "only one slot: 1");
}

TEST_CASE("the truncated flag on the wire record surfaces on the decoded record")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 1024u, 1u, 1u, 1u, 0u, 0u);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), 1u);

    const auto defPayload =
        buildSiteDefinitionPayload(0x33u, 0u, 1u, Severity::Warning, {}, "cut short", "b.cpp");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SiteDefinition, 0, 0, defPayload);

    wire::MessagePayload msg{};
    msg.siteId_ = 0x33u;
    std::vector<std::byte> payload;
    appendRaw(payload, msg);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, wire::cFlagTruncated, 1, payload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());
    Decoder decoder;
    const std::vector<DecodedRecord> records = decoder.decodeAll(reader);

    REQUIRE(records.size() == 1u);
    CHECK(records[0].truncated_);
}
