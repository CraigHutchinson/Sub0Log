// SegmentReader and Decoder: raw record recovery and typed decoding.
// Segment images are built BY HAND from wire.hpp primitives -- deliberately
// independent of the producer, per the reader's contract (docs/architecture.md:
// "a reader never touches producer state").

#include <sub0log/reader.hpp>

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace sub0log;

namespace {

// ---------------------------------------------------------------------------
// Byte-level helpers: append a value's wire representation to a buffer.

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

/// u16 length + bytes, as SiteDefinition's format/file tail and a Bytes arg.
void appendLengthPrefixed(std::vector<std::byte>& buf, const std::string_view s)
{
    appendRaw<std::uint16_t>(buf, static_cast<std::uint16_t>(s.size()));
    appendBytesRaw(buf, s);
}

std::vector<std::byte> buildSiteDefinitionPayload(
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

// ---------------------------------------------------------------------------
// SegmentImageBuilder: assembles a valid segment image byte by byte.

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

    [[nodiscard]] std::uint32_t chunkBytes() const noexcept { return chunkBytes_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] std::uint64_t chunkOffset(const std::uint32_t index) const noexcept
    {
        return headerBytes_ + static_cast<std::uint64_t>(index) * chunkBytes_;
    }

    /// Stamps a chunk's header; returns the offset of the chunk's record body.
    std::uint64_t stampChunk(const std::uint32_t index, const std::uint64_t generation,
                              const std::uint64_t ownerThread, const std::uint64_t claimMonoNs = 0)
    {
        const std::uint64_t offset = chunkOffset(index);
        wire::ChunkHeader ch{generation, ownerThread, claimMonoNs, 0};
        wire::storeUnaligned(image_.data() + offset, ch);
        return offset + sizeof(wire::ChunkHeader);
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

} // namespace

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

    // Trailing chunk space beyond the last record is unwritten (zero, never
    // committed) and must be reported, not silently ignored (R3.3/R9.2).
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(reader.unreadableBytes() == bodyBytes - consumed);
    (void)recordStart;
}

// ---------------------------------------------------------------------------
// (2) Truncated tail: an uncommitted (zero) head word mid-chunk.

TEST_CASE("an uncommitted head word stops the chunk and counts the remainder exactly")
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
    const std::uint64_t unreadable = reader.visit([&](const RecordView&) { ++yielded; });

    CHECK(yielded == 1u);
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(unreadable == bodyBytes - consumed);
    CHECK(reader.unreadableBytes() == unreadable);
}

// ---------------------------------------------------------------------------
// (3) Torn head word: garbage without the commit tag.

TEST_CASE("a torn head word is contained exactly like an uncommitted one")
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
    const std::uint64_t unreadable = reader.visit([&](const RecordView&) { ++yielded; });

    CHECK(yielded == 1u);
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(unreadable == bodyBytes - consumed);
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
    const std::uint64_t unreadable = reader.visit([&](const RecordView&) { ++yielded; });

    CHECK(yielded == 0u); // never yielded: it would have run outside the chunk
    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(unreadable == bodyBytes); // whole remainder from the head word onward
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
    const std::uint64_t unreadable =
        reader.visit([&](const RecordView& v) { chunkIndices.push_back(v.chunkIndex_); });

    REQUIRE(chunkIndices.size() == 1u);
    CHECK(chunkIndices[0] == 0u);

    const std::uint64_t bodyBytes = builder.chunkBytes() - sizeof(wire::ChunkHeader);
    CHECK(unreadable == (bodyBytes - consumed0) + bodyBytes);
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
