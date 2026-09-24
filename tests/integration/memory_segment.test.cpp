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
    constexpr std::size_t cBytes = sub0log::wire::cSegmentHeaderBytes + 3u * cChunkBytes;
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
            std::span<std::byte>{storage}.first(sub0log::wire::cSegmentHeaderBytes), smallChunks());
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
