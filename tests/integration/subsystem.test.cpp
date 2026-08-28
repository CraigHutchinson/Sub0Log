// SubsystemDefinition: a segment naming its own subsystems (docs/adoption-
// friction.md 2.2), so a decoder run years later against a lone file needs
// no header nobody kept. Producer-side tests round-trip through a real file
// (Logger + Decoder, matching system/roundtrip.test.cpp's style); the
// backward-compatibility claim is checked against a hand-built image, the
// same independent-of-the-producer style reader.test.cpp uses.

#include <sub0log/log.hpp>
#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"
#include "support/segment_image.hpp"

#include "support/test_framework.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sub0log;
using namespace sub0log::test;

namespace {

constexpr SubsystemId cStorage{1};
constexpr SubsystemId cNetwork{2};
constexpr SubsystemId cPlugin{5};
constexpr SubsystemId cUndeclared{99};

/// wire::SubsystemDefinitionPayload's fixed prefix, then the (already
/// length-fitting) name -- the hand-built-image counterpart to
/// buildSiteDefinitionPayload in segment_image.hpp, kept local because that
/// file is other work's, not this workstream's, to edit.
std::vector<std::byte> buildSubsystemDefinitionPayload(const std::uint32_t subsystemId,
                                                        const std::string_view name)
{
    std::vector<std::byte> buf;
    wire::SubsystemDefinitionPayload prefix{};
    prefix.subsystemId_ = subsystemId;
    prefix.nameLen_ = static_cast<std::uint16_t>(name.size());
    prefix.reserved0_ = 0u;
    appendRaw(buf, prefix);
    appendBytesRaw(buf, name);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) Options-declared names come back after a real round trip through a
//     file, and the messages that used those subsystems decode normally.

TEST_CASE("subsystem names declared in Options round-trip through a file")
{
    const auto directory = freshDirectory("subsys-options");

    const std::pair<SubsystemId, std::string_view> declared[] = {
        {cStorage, "Storage"},
        {cNetwork, "Network"},
    };

    {
        auto logger = Logger::create({.directory_ = directory.string(),
                                      .subsystemNames_ = declared});
        REQUIRE(logger.valid());
        Logger::ScopedBind bind{logger};
        sub0log_info(cStorage, "opened");
        sub0log_info(cNetwork, "connected");

        const auto stats = logger.stats();
        CHECK(stats.droppedRecords_ == 0u);
        CHECK(stats.truncatedRecords_ == 0u);
    }

    const auto image = slurp(onlySegmentIn(directory));
    auto reader = SegmentReader::open(image);
    REQUIRE(reader.valid());

    Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);
    REQUIRE(records.size() == 2u);

    CHECK(decoder.subsystemName(cStorage) == "Storage");
    CHECK(decoder.subsystemName(cNetwork) == "Network");

    std::filesystem::remove_all(directory);
}

// ---------------------------------------------------------------------------
// (2) declareSubsystem() after construction: a plugin registering itself.

TEST_CASE("Logger::declareSubsystem after construction names a subsystem the same way")
{
    const auto directory = freshDirectory("subsys-declare");

    {
        auto logger = Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        Logger::ScopedBind bind{logger};

        logger.declareSubsystem(cPlugin, "Plugin");
        sub0log_info(cPlugin, "registered");

        CHECK(logger.stats().droppedRecords_ == 0u);
    }

    const auto image = slurp(onlySegmentIn(directory));
    auto reader = SegmentReader::open(image);
    REQUIRE(reader.valid());

    Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);
    REQUIRE(records.size() == 1u);
    CHECK(decoder.subsystemName(cPlugin) == "Plugin");

    std::filesystem::remove_all(directory);
}

// ---------------------------------------------------------------------------
// (3) An undeclared subsystem: the honest empty answer, and the message it
//     labels still decodes -- the pre-existing-segment case.

TEST_CASE("an undeclared subsystem's name is empty, and its records still decode")
{
    const auto directory = freshDirectory("subsys-undeclared");

    {
        auto logger = Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        Logger::ScopedBind bind{logger};
        sub0log_info(cUndeclared, "no name for this one");
    }

    const auto image = slurp(onlySegmentIn(directory));
    auto reader = SegmentReader::open(image);
    REQUIRE(reader.valid());

    Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);
    REQUIRE(records.size() == 1u);
    CHECK(records[0].site_->subsystem_ == cUndeclared);

    CHECK(decoder.subsystemName(cUndeclared).empty());

    std::filesystem::remove_all(directory);
}

// ---------------------------------------------------------------------------
// (4) A name past the cap is truncated, visibly (R9.2), never silently.

TEST_CASE("a subsystem name longer than the inline cap is truncated, and the cut is counted")
{
    const auto directory = freshDirectory("subsys-truncated");
    const std::string longName(static_cast<std::size_t>(wire::cInlineBytesCap) + 50u, 'x');

    Logger::Stats stats{};
    {
        auto logger = Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        Logger::ScopedBind bind{logger};
        logger.declareSubsystem(cPlugin, longName);
        stats = logger.stats();
    }

    // R9.2: a cut is visible, never silent -- the same counter
    // writeSiteDefinition's own truncation raises.
    CHECK(stats.truncatedRecords_ == 1u);

    const auto image = slurp(onlySegmentIn(directory));
    auto reader = SegmentReader::open(image);
    REQUIRE(reader.valid());

    Decoder decoder;
    (void)decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);

    const std::string_view name = decoder.subsystemName(cPlugin);
    CHECK(name.size() == wire::cInlineBytesCap);
    CHECK(name == std::string_view(longName).substr(0, wire::cInlineBytesCap));

    std::filesystem::remove_all(directory);
}

// ---------------------------------------------------------------------------
// (5) A duplicate declaration keeps the first, exactly as a duplicate
//     SiteDefinition does.

TEST_CASE("a duplicate subsystem declaration keeps the first name")
{
    const auto directory = freshDirectory("subsys-duplicate");

    {
        auto logger = Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        Logger::ScopedBind bind{logger};
        logger.declareSubsystem(cPlugin, "First");
        logger.declareSubsystem(cPlugin, "Second");
    }

    const auto image = slurp(onlySegmentIn(directory));
    auto reader = SegmentReader::open(image);
    REQUIRE(reader.valid());

    Decoder decoder;
    (void)decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);
    CHECK(decoder.subsystemName(cPlugin) == "First");

    std::filesystem::remove_all(directory);
}

// ---------------------------------------------------------------------------
// (6) The backward-compatibility claim: a segment carrying subsystem
//     definitions decodes with zero undecodable records -- a
//     SubsystemDefinition must never read as damage. Hand-built, independent
//     of the producer, per reader.test.cpp's convention.

TEST_CASE("a segment containing subsystem definitions decodes with no damage")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 1024u, 1u, /*generation=*/55u,
                                /*processId=*/1u, /*anchorMonoNs=*/0u, /*anchorWallNs=*/0u);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), /*ownerThread=*/1u);

    const auto subsystemPayload = buildSubsystemDefinitionPayload(cStorage.value_, "Storage");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SubsystemDefinition, subsystemPayload);

    const auto sitePayload = buildSiteDefinitionPayload(0x77u, cStorage.value_, 1u,
                                                         Severity::Info, {}, "hello", "f.cpp");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SiteDefinition, sitePayload);

    wire::MessagePayload msg{};
    msg.siteId_ = 0x77u;
    std::vector<std::byte> msgPayload;
    appendRaw(msgPayload, msg);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, msgPayload);
    (void)cursor;

    SegmentReader reader = SegmentReader::open(builder.span());
    REQUIRE(reader.valid());

    Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    // The crux of the claim: known to the decoder, not merely tolerated --
    // it neither breaks decoding nor is it counted as unrecognised.
    CHECK(decoder.undecodableRecords() == 0u);
    CHECK(decoder.skippedRecords() == 0u);
    REQUIRE(records.size() == 1u);
    CHECK(decoder.subsystemName(cStorage) == "Storage");
}

// ---------------------------------------------------------------------------
// (7) Merger: names declared in one segment resolve for a merged record
//     that carries no back-pointer to which segment it came from.

TEST_CASE("Merger::subsystemName resolves a name across the segments it was given")
{
    SegmentImageBuilder builder(wire::cSegmentHeaderBytes, 1024u, 1u, /*generation=*/9u,
                                /*processId=*/1u, /*anchorMonoNs=*/0u, /*anchorWallNs=*/0u);
    std::uint64_t cursor = builder.stampChunk(0, builder.generation(), 1u);

    const auto subsystemPayload = buildSubsystemDefinitionPayload(cNetwork.value_, "Network");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SubsystemDefinition, subsystemPayload);

    const auto sitePayload = buildSiteDefinitionPayload(0x88u, cNetwork.value_, 1u,
                                                         Severity::Info, {}, "up", "n.cpp");
    cursor = builder.writeRecord(cursor, wire::RecordKind::SiteDefinition, sitePayload);

    wire::MessagePayload msg{};
    msg.siteId_ = 0x88u;
    std::vector<std::byte> msgPayload;
    appendRaw(msgPayload, msg);
    cursor = builder.writeRecord(cursor, wire::RecordKind::Message, msgPayload);
    (void)cursor;

    Merger merger;
    REQUIRE(merger.addSegment(builder.span()) == SegmentError::Ok);

    const auto merged = merger.merged();
    REQUIRE(merged.size() == 1u);
    CHECK(merger.subsystemName(cNetwork) == "Network");
    CHECK(merger.subsystemName(cUndeclared).empty());
    CHECK(merger.totals().undecodableRecords_ == 0u);
}
