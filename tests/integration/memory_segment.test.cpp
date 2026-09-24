// Logger::createInMemory (issue #2, docs/vnext-backends-and-memory.md
// rung 1): the same producer writing the same wire format into memory the
// caller owns. Everything here reads back through the unmodified
// SegmentReader/Decoder -- the claim under test is that nothing downstream
// can tell the bytes never came from a file.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include "support/test_framework.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cEmbedded{5};

// Small, so each test sizes its own exhaustion point: header plus a
// handful of minimum-sized chunks.
constexpr std::uint32_t cChunkBytes = sub0log::wire::cChunkSizeUnit;

sub0log::Logger::Options smallChunks()
{
    sub0log::Logger::Options options{};
    options.segment_.chunkBytes_ = cChunkBytes;
    return options;
}

} // namespace

TEST_CASE("an in-memory Logger round-trips typed records through the caller's buffer")
{
    alignas(8) static std::array<std::byte, 64 * 1024> storage{};

    const std::pair<sub0log::SubsystemId, std::string_view> names[] = {{cEmbedded, "embedded"}};
    auto options = smallChunks();
    options.subsystemNames_ = names;

    auto logger = sub0log::Logger::createInMemory(storage, options);
    REQUIRE(logger.valid());
    CHECK(logger.segmentPath().empty()); // there is no file

    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cEmbedded, "sample {} of {}", std::uint32_t{3}, std::string_view{"imu"});
        sub0log_error(cEmbedded, "no args");
    }

    auto reader = sub0log::SegmentReader::open(storage);
    REQUIRE(reader.valid());
    CHECK(reader.unreadableBytes() == 0);
    CHECK(reader.header().segmentBytes_ == storage.size());
    CHECK(reader.header().chunkBytes_ == cChunkBytes);

    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0);
    REQUIRE(records.size() == 2);
    CHECK(sub0log::Decoder::format(records[0]) == "sample 3 of imu");
    CHECK(records[0].site_->subsystem_ == cEmbedded);
    CHECK(records[1].site_->severity_ == sub0log::Severity::Error);
    CHECK(decoder.subsystemName(cEmbedded) == "embedded");
    CHECK(logger.stats().droppedRecords_ == 0);
}

TEST_CASE("an in-memory Logger over a previous run's buffer shows only the new run")
{
    // Caller memory is not a freshly truncated file: a warm-reset RAM
    // region or a reused pool buffer still holds the last run's committed
    // records, which read as perfectly valid unless creation clears them.
    alignas(8) static std::array<std::byte, 16 * 1024> storage{};
    {
        auto previous = sub0log::Logger::createInMemory(storage, smallChunks());
        REQUIRE(previous.valid());
        sub0log::Logger::ScopedBind bind{previous};
        for (std::uint64_t i = 0; i < 20; ++i) {
            sub0log_info(cEmbedded, "old {}", i);
        }
    }

    auto logger = sub0log::Logger::createInMemory(storage, smallChunks());
    REQUIRE(logger.valid());
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cEmbedded, "fresh {}", std::uint64_t{1});
    }

    auto reader = sub0log::SegmentReader::open(storage);
    REQUIRE(reader.valid());
    CHECK(reader.unreadableBytes() == 0);
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0);
    REQUIRE(records.size() == 1);
    CHECK(sub0log::Decoder::format(records[0]) == "fresh 1");
}

TEST_CASE("a full in-memory buffer drops and counts, and every record is accounted for")
{
    // Header plus exactly three chunks: one thread holds one chunk at a
    // time, so this exhausts after a known, small number of records.
    constexpr std::size_t cBytes = sub0log::wire::cCompactSegmentHeaderBytes + 3u * cChunkBytes;
    alignas(8) static std::array<std::byte, cBytes> storage{};

    auto logger = sub0log::Logger::createInMemory(storage, smallChunks());
    REQUIRE(logger.valid());

    constexpr std::uint64_t cEmitted = 500;
    {
        sub0log::Logger::ScopedBind bind{logger};
        for (std::uint64_t i = 0; i < cEmitted; ++i) {
            sub0log_info(cEmbedded, "tick {}", i);
        }
    }

    const auto stats = logger.stats();
    CHECK(stats.droppedRecords_ > 0);

    auto reader = sub0log::SegmentReader::open(storage);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0);
    CHECK(reader.unreadableBytes() == 0);

    // R9.1: nothing vanishes silently. Every emit is either decodable or
    // counted, and what was kept is the oldest prefix, in order.
    CHECK(records.size() + stats.droppedRecords_ == cEmitted);
    REQUIRE(!records.empty());
    for (std::size_t i = 0; i < records.size(); ++i) {
        CHECK(std::get<std::uint64_t>(records[i].args_[0]) == i);
    }
}

TEST_CASE("an in-memory Logger refuses storage it cannot use, and says why")
{
    alignas(8) static std::array<std::byte, 8 * 1024> storage{};

    SUBCASE("misaligned")
    {
        auto logger = sub0log::Logger::createInMemory(
            std::span<std::byte>{storage}.subspan(4), smallChunks());
        CHECK_FALSE(logger.valid());
        CHECK(logger.error());
    }
    SUBCASE("smaller than the header plus one chunk")
    {
        auto logger = sub0log::Logger::createInMemory(
            std::span<std::byte>{storage}.first(sub0log::wire::cCompactSegmentHeaderBytes), smallChunks());
        CHECK_FALSE(logger.valid());
        CHECK(logger.error());
    }
    SUBCASE("empty")
    {
        auto logger = sub0log::Logger::createInMemory({}, smallChunks());
        CHECK_FALSE(logger.valid());
        CHECK(logger.error());
    }
    SUBCASE("binding an invalid one still counts rather than crashing")
    {
        auto logger = sub0log::Logger::createInMemory({}, smallChunks());
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cEmbedded, "into nothing");
        CHECK(logger.stats().droppedRecords_ == 1);
    }
}

TEST_CASE("moving an in-memory Logger leaves exactly one owner of the buffer")
{
    alignas(8) static std::array<std::byte, 16 * 1024> storage{};
    auto first = sub0log::Logger::createInMemory(storage, smallChunks());
    REQUIRE(first.valid());
    auto second = std::move(first);
    CHECK(second.valid());
    CHECK_FALSE(first.valid()); // NOLINT(bugprone-use-after-move): the point of the test
}

TEST_CASE("usage() reports chunks claimed, clamped at the segment's total")
{
    // 1 KiB chunks: at the 128-byte minimum a new site's definition record
    // (it carries the full __FILE__ path) does not share a chunk with its
    // first message, and the count below would be 3, correctly.
    constexpr std::uint32_t cUsageChunkBytes = 1024u;
    constexpr std::size_t cBytes = sub0log::wire::cCompactSegmentHeaderBytes + 4u * cUsageChunkBytes;
    alignas(8) static std::array<std::byte, cBytes> storage{};
    sub0log::Logger::Options options{};
    options.segment_.chunkBytes_ = cUsageChunkBytes;
    auto logger = sub0log::Logger::createInMemory(storage, options);
    REQUIRE(logger.valid());
    CHECK(logger.usage().chunksClaimed_ == 0);
    CHECK(logger.usage().chunkCount_ == 4);

    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cEmbedded, "one");
        CHECK(logger.usage().chunksClaimed_ == 1);
        // Far past exhaustion: every refused refill still bumps the raw
        // cursor, which is exactly what usage() must not report.
        for (std::uint64_t i = 0; i < 1000; ++i) {
            sub0log_info(cEmbedded, "fill {}", i);
        }
    }
    CHECK(logger.stats().droppedRecords_ > 0);
    CHECK(logger.usage().chunksClaimed_ == 4);

    auto invalid = sub0log::Logger::createInMemory({}, smallChunks());
    CHECK(invalid.usage().chunksClaimed_ == 0);
    CHECK(invalid.usage().chunkCount_ == 0);
}

TEST_CASE("post-mortem: the previous boot's buffer is read before the next Logger reuses it")
{
    // The retained-RAM recipe (docs/embedded.md): createInMemory() zeroes
    // its storage, so whatever survived a reset has to be read -- or copied
    // out to NVM -- first. This is the order that works.
    alignas(8) static std::array<std::byte, 16 * 1024> retained{};
    {
        auto previousBoot = sub0log::Logger::createInMemory(retained, smallChunks());
        REQUIRE(previousBoot.valid());
        sub0log::Logger::ScopedBind bind{previousBoot};
        sub0log_error(cEmbedded, "last words {}", std::uint32_t{0xDEAD});
        // ...and the device resets here, without anything tearing down.
    }

    auto survivor = sub0log::SegmentReader::open(retained);
    REQUIRE(survivor.valid());
    sub0log::Decoder decoder;
    const auto lastBoot = decoder.decodeAll(survivor);
    REQUIRE(lastBoot.size() == 1);
    CHECK(sub0log::Decoder::format(lastBoot[0]) == "last words 57005");
    const std::uint64_t lastGeneration = survivor.header().generation_;

    auto thisBoot = sub0log::Logger::createInMemory(retained, smallChunks());
    REQUIRE(thisBoot.valid());
    CHECK(thisBoot.segmentGeneration() != lastGeneration);
}

TEST_CASE("A/B in-memory segments drained to a custom sink lose nothing")
{
    // The shape for any endpoint that is not byte-addressable RAM -- NOR
    // flash, an SPI/I2C part, a UART, a radio (docs/embedded.md, "Custom
    // endpoints"): producers only ever write RAM; a control task watches
    // usage(), swaps the binding to the spare segment, and hands the full
    // one to the sink whole. The sink here is a vector of images standing
    // in for flash pages; what matters is that the images, read back with
    // the ordinary reader in the order the sink received them, account for
    // every record in order. Sink order, not Merger: Merger aligns each
    // segment through its own anchor pair, and two anchor pairs sampled
    // milliseconds apart misorder a single producer's records at the
    // boundary by the sampling error (docs/embedded.md, "Ordering across
    // rotated segments").
    constexpr std::size_t cBytes = sub0log::wire::cCompactSegmentHeaderBytes + 6u * cChunkBytes;
    alignas(8) static std::array<std::byte, cBytes> bufferA{};
    alignas(8) static std::array<std::byte, cBytes> bufferB{};
    std::array<std::span<std::byte>, 2> buffers{bufferA, bufferB};

    std::vector<std::vector<std::byte>> sink;
    std::uint64_t dropped = 0;

    constexpr std::uint64_t cEmitted = 3000;
    std::size_t current = 0;
    std::uint64_t next = 0;
    while (next < cEmitted) {
        auto logger = sub0log::Logger::createInMemory(buffers[current], smallChunks());
        REQUIRE(logger.valid());
        {
            sub0log::Logger::ScopedBind bind{logger};
            // Rotate one chunk before the end: a single producer holds at
            // most one chunk, so this watermark cannot drop.
            while (next < cEmitted
                   && logger.usage().chunksClaimed_ < logger.usage().chunkCount_) {
                sub0log_info(cEmbedded, "seq {}", next);
                ++next;
            }
        }
        // Unbound: no producer can still be writing here, so the whole
        // image is stable. (With live producer threads the caller needs a
        // quiescent point before *reusing* the buffer -- docs/embedded.md.)
        dropped += logger.stats().droppedRecords_;
        sink.emplace_back(buffers[current].begin(), buffers[current].end());
        current ^= 1u;
    }
    CHECK(dropped == 0);
    CHECK(sink.size() > 2); // it really did rotate, and reused both buffers

    std::uint64_t expected = 0;
    bool inOrder = true;
    for (const auto& image : sink) {
        auto reader = sub0log::SegmentReader::open(image);
        REQUIRE(reader.valid());
        CHECK(reader.unreadableBytes() == 0);
        sub0log::Decoder decoder;
        for (const auto& record : decoder.decodeAll(reader)) {
            inOrder = inOrder && std::get<std::uint64_t>(record.args_[0]) == expected;
            ++expected;
        }
        CHECK(decoder.undecodableRecords() == 0);
    }
    CHECK(inOrder);
    CHECK(expected == cEmitted);
}

TEST_CASE("an in-memory segment's header area is compact, and readers take it from the header")
{
    // 4096 bytes of header is a file mapping's page; on a 16 KiB buffer it
    // would be a quarter of the memory. The size is data in the header, so
    // the ordinary reader needs nothing to find chunk 0 at 192.
    alignas(8) static std::array<std::byte, 2048> storage{};
    auto logger = sub0log::Logger::createInMemory(storage, smallChunks());
    REQUIRE(logger.valid());
    CHECK(logger.usage().chunkCount_
          == (storage.size() - sub0log::wire::cCompactSegmentHeaderBytes) / cChunkBytes);
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cEmbedded, "tiny {}", std::uint32_t{1});
    }
    auto reader = sub0log::SegmentReader::open(storage);
    REQUIRE(reader.valid());
    CHECK(reader.header().headerBytes_ == sub0log::wire::cCompactSegmentHeaderBytes);
    CHECK(reader.unreadableBytes() == 0);
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    REQUIRE(records.size() == 1);
    CHECK(sub0log::Decoder::format(records[0]) == "tiny 1");
}
