// The producer path end to end: Segment/claimChunk, ChunkWriter reserve and
// commit, and the Logger + call-site macro writing bytes a test can check
// by hand against wire.hpp. Deliberately does not include reader.hpp --
// that is the other half of the contract, verified independently.

#include <sub0log/chunk.hpp>
#include <sub0log/log.hpp>
#include <sub0log/segment.hpp>
#include <sub0log/wire.hpp>

#include "support/test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_TEST_GETPID() static_cast<int>(::_getpid())
#else
#  include <unistd.h>
#  define SUB0LOG_TEST_GETPID() static_cast<int>(::getpid())
#endif

using namespace sub0log;

namespace {

/// A fresh, empty temp directory for one test's segment file(s). Under this
/// session's scratchpad when available, falling back to the system temp
/// directory (portable, and what a consumer's own test run would use).
std::string makeTempDir()
{
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::filesystem::path candidate =
            base / ("sub0log-test-" + std::to_string(SUB0LOG_TEST_GETPID()) + "-" + std::to_string(attempt));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) {
            return candidate.string();
        }
    }
    FAIL("could not create a temp directory for a producer test");
    return {};
}

std::vector<std::byte> readFile(const std::string& path, std::size_t bytes)
{
    std::vector<std::byte> buf(bytes);
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(bytes));
    REQUIRE(in.good());
    return buf;
}

constexpr SubsystemId TestSubsystem{42};

} // namespace

// ---------------------------------------------------------------------------
// ChunkWriter, in isolation (no Segment/file involved).

TEST_CASE("ChunkWriter reserve/commit produces a readable committed record; "
         "an uncommitted reservation leaves a zero head word")
{
    std::vector<std::byte> storage(256, std::byte{0});
    detail::ChunkWriter writer{std::span<std::byte>(storage)};

    REQUIRE(writer.valid());
    CHECK(writer.remaining() == 256u);

    const auto slot = writer.reserve(10u);
    REQUIRE(slot.valid());
    CHECK(slot.payload_ == slot.headWord_ + 8);

    // Uncommitted: the head word is exactly the zero-fill it started as.
    CHECK(wire::loadUnaligned<std::uint64_t>(slot.headWord_) == 0u);
    CHECK_FALSE(wire::RecordHead::isCommitted(wire::loadUnaligned<std::uint64_t>(slot.headWord_)));

    for (int i = 0; i < 10; ++i) {
        slot.payload_[i] = static_cast<std::byte>(0xA0 + i);
    }

    wire::RecordHead head{};
    head.payloadBytes_ = 10u;
    head.kind_ = wire::RecordKind::Message;
    head.flags_ = wire::cFlagTruncated;
    writer.commit(slot, head);

    const std::uint64_t committedWord = wire::loadUnaligned<std::uint64_t>(slot.headWord_);
    REQUIRE(wire::RecordHead::isCommitted(committedWord));
    const wire::RecordHead back = wire::RecordHead::unpack(committedWord);
    CHECK(back.payloadBytes_ == 10u);
    CHECK(back.kind_ == wire::RecordKind::Message);
    CHECK(back.flags_ == wire::cFlagTruncated);
    CHECK(back.sequence_ == 0u); // first record committed on this chunk

    for (int i = 0; i < 10; ++i) {
        CHECK(slot.payload_[i] == static_cast<std::byte>(0xA0 + i));
    }

    // A second reservation's head word is still zero until its own commit.
    const auto slot2 = writer.reserve(4u);
    REQUIRE(slot2.valid());
    CHECK(wire::loadUnaligned<std::uint64_t>(slot2.headWord_) == 0u);
    writer.commit(slot2, wire::RecordHead{.payloadBytes_ = 4u, .kind_ = wire::RecordKind::Message});
    const wire::RecordHead back2 =
        wire::RecordHead::unpack(wire::loadUnaligned<std::uint64_t>(slot2.headWord_));
    CHECK(back2.sequence_ == 1u); // sequence advances per commit
}

TEST_CASE("ChunkWriter.reserve fails, without corrupting state, once the chunk cannot fit")
{
    std::vector<std::byte> storage(24, std::byte{0}); // room for exactly one 8+8 record
    detail::ChunkWriter writer{std::span<std::byte>(storage)};

    const auto ok = writer.reserve(8u); // 8 (head) + paddedPayload(8)=8 -> 16, fits in 24
    REQUIRE(ok.valid());
    CHECK(writer.remaining() == 8u);

    const auto tooBig = writer.reserve(1u); // needs 8 + 8 = 16, only 8 left
    CHECK_FALSE(tooBig.valid());
    CHECK(writer.remaining() == 8u); // the failed reserve did not consume anything
}

// ---------------------------------------------------------------------------
// Segment: file creation, header contents, chunk claiming and exhaustion.

TEST_CASE("Segment::create writes a header whose fields agree with the file it produced")
{
    const std::string dir = makeTempDir();
    detail::SegmentOptions opts{};
    opts.chunkBytes_ = wire::cDefaultChunkBytes;
    opts.segmentBytes_ = wire::cSegmentHeaderBytes + 2u * opts.chunkBytes_;

    auto segment = detail::Segment::create(dir, "seg", opts);
    REQUIRE(segment.valid());
    CHECK_FALSE(segment.error());

    CHECK(std::filesystem::file_size(segment.path()) == opts.segmentBytes_);

    const auto bytes = readFile(segment.path(), wire::cSegmentHeaderBytes);
    const auto header = wire::loadUnaligned<wire::SegmentHeader>(bytes.data());

    CHECK(header.magic_ == wire::cMagic);
    CHECK(header.formatVersion_ == wire::cFormatVersion);
    CHECK(header.headerBytes_ == wire::cSegmentHeaderBytes);
    CHECK(header.chunkBytes_ == opts.chunkBytes_);
    CHECK(header.segmentBytes_ == opts.segmentBytes_);
    CHECK(header.generation_ == segment.generation());
    CHECK(header.generation_ != 0u); // R3.4: non-zero generation
    CHECK(header.processId_ != 0u);

    const std::uint64_t cursor = wire::loadUnaligned<std::uint64_t>(bytes.data() + wire::cNextChunkOffset);
    CHECK(cursor == 0u);
}

TEST_CASE("claimChunk hands out distinct, correctly-stamped chunks and rejects exhaustion")
{
    const std::string dir = makeTempDir();
    detail::SegmentOptions opts{};
    opts.chunkBytes_ = 256u;
    opts.segmentBytes_ = wire::cSegmentHeaderBytes + 2u * opts.chunkBytes_;

    auto segment = detail::Segment::create(dir, "seg", opts);
    REQUIRE(segment.valid());

    auto w0 = segment.claimChunk();
    CHECK(w0.valid());
    auto w1 = segment.claimChunk();
    CHECK(w1.valid());
    auto w2 = segment.claimChunk(); // segment has only 2 chunks
    CHECK_FALSE(w2.valid());

    const auto bytes = readFile(segment.path(), static_cast<std::size_t>(opts.segmentBytes_));
    const std::byte* const chunk0 = bytes.data() + wire::cSegmentHeaderBytes;
    const std::byte* const chunk1 = chunk0 + opts.chunkBytes_;

    const auto h0 = wire::loadUnaligned<wire::ChunkHeader>(chunk0);
    const auto h1 = wire::loadUnaligned<wire::ChunkHeader>(chunk1);

    CHECK(h0.generation_ == segment.generation());
    CHECK(h1.generation_ == segment.generation());
    CHECK(h0.ownerThread_ != 0u);
    CHECK(h0.ownerThread_ == h1.ownerThread_); // same (this) thread claimed both
}

// ---------------------------------------------------------------------------
// Logger + the call-site macro: the whole producer path, checked by hand.

TEST_CASE("Logger + sub0log_info writes a SiteDefinition then a Message a test can verify by hand")
{
    const std::string dir = makeTempDir();
    Logger::Options options{};
    options.directory_ = dir;
    options.stem_ = "prodtest";
    options.segment_.chunkBytes_ = 4096u;
    options.segment_.segmentBytes_ = wire::cSegmentHeaderBytes + 2u * options.segment_.chunkBytes_;
    options.threshold_ = Severity::Trace;

    auto logger = Logger::create(options);
    REQUIRE(logger.valid());

    {
        Logger::ScopedBind bind{logger};
        sub0log_info(TestSubsystem, "value={}", 42);
    }

    const auto stats = logger.stats();
    CHECK(stats.droppedRecords_ == 0u);
    CHECK(stats.truncatedRecords_ == 0u);

    const auto bytes = readFile(logger.segmentPath(), static_cast<std::size_t>(options.segment_.segmentBytes_));
    const std::byte* const chunkBody =
        bytes.data() + wire::cSegmentHeaderBytes + sizeof(wire::ChunkHeader);

    // Record 1: the SiteDefinition, written because this is the site's
    // first use in this process (docs/record-model.md).
    const std::uint64_t defHead = wire::loadUnaligned<std::uint64_t>(chunkBody);
    REQUIRE(wire::RecordHead::isCommitted(defHead));
    const wire::RecordHead defRecord = wire::RecordHead::unpack(defHead);
    CHECK(defRecord.kind_ == wire::RecordKind::SiteDefinition);
    CHECK(defRecord.sequence_ == 0u);
    CHECK((defRecord.flags_ & wire::cFlagTruncated) == 0u);

    const std::byte* const defPayload = chunkBody + 8;
    const auto defFields = wire::loadUnaligned<wire::SiteDefinitionPayload>(defPayload);
    CHECK(defFields.siteId_ != 0u);
    CHECK(defFields.subsystemId_ == TestSubsystem.value_);
    CHECK(defFields.severity_ == static_cast<std::uint8_t>(Severity::Info));
    CHECK(defFields.argCount_ == 1u);

    const std::byte* const typeCodes = defPayload + sizeof(wire::SiteDefinitionPayload);
    CHECK(static_cast<wire::TypeCode>(std::to_integer<std::uint8_t>(typeCodes[0])) == wire::TypeCode::I32);

    const std::byte* const formatLenPtr = typeCodes + defFields.argCount_;
    const std::uint16_t formatLen = wire::loadUnaligned<std::uint16_t>(formatLenPtr);
    const std::string format(reinterpret_cast<const char*>(formatLenPtr + 2), formatLen);
    CHECK(format == "value={}");

    const std::byte* const fileLenPtr = formatLenPtr + 2 + formatLen;
    const std::uint16_t fileLen = wire::loadUnaligned<std::uint16_t>(fileLenPtr);
    CHECK(fileLen > 0u); // __FILE__, contents not pinned to this test file's path

    // Record 2: the Message, right after the (8-byte-padded) SiteDefinition.
    const std::uint32_t defTotal = 8u + wire::paddedPayload(defRecord.payloadBytes_);
    const std::byte* const msgHeadPtr = chunkBody + defTotal;
    const std::uint64_t msgHead = wire::loadUnaligned<std::uint64_t>(msgHeadPtr);
    REQUIRE(wire::RecordHead::isCommitted(msgHead));
    const wire::RecordHead msgRecord = wire::RecordHead::unpack(msgHead);
    CHECK(msgRecord.kind_ == wire::RecordKind::Message);
    CHECK(msgRecord.sequence_ == 1u);
    CHECK((msgRecord.flags_ & wire::cFlagTruncated) == 0u);
    CHECK(msgRecord.payloadBytes_ == sizeof(wire::MessagePayload) + 4u); // one i32 argument

    const std::byte* const msgPayload = msgHeadPtr + 8;
    const auto msgFields = wire::loadUnaligned<wire::MessagePayload>(msgPayload);
    CHECK(msgFields.siteId_ == defFields.siteId_); // same call site
    CHECK(msgFields.correlationId_ == 0u);          // no CorrelationScope entered

    const std::int32_t argValue =
        wire::loadUnaligned<std::int32_t>(msgPayload + sizeof(wire::MessagePayload));
    CHECK(argValue == 42);
}

TEST_CASE("Logger counts drops once a tiny segment is exhausted")
{
    const std::string dir = makeTempDir();
    Logger::Options options{};
    options.directory_ = dir;
    options.stem_ = "droptest";
    options.segment_.chunkBytes_ = 128u; // minimum-viable: fits a few small records
    options.segment_.segmentBytes_ = wire::cSegmentHeaderBytes + 2u * options.segment_.chunkBytes_;
    options.threshold_ = Severity::Trace;

    auto logger = Logger::create(options);
    REQUIRE(logger.valid());

    {
        Logger::ScopedBind bind{logger};
        for (int i = 0; i < 200; ++i) {
            sub0log_info(TestSubsystem, "n={}", i);
        }
    }

    const auto stats = logger.stats();
    CHECK(stats.droppedRecords_ > 0u); // R9.1: dropped records are counted
}

TEST_CASE("with no Logger bound, the macro is a no-op and never evaluates its arguments")
{
    REQUIRE(Logger::active() == nullptr); // nothing bound at this point in the suite

    bool evaluated = false;
    const auto sideEffect = [&evaluated]() -> int {
        evaluated = true;
        return 7;
    };
    sub0log_info(TestSubsystem, "value={}", sideEffect());

    CHECK_FALSE(evaluated); // R1.4: a disabled site does not evaluate its arguments
}
