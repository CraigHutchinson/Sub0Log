// Paths a user actually has: the ones with their own name in them.
//
// docs/adoption-friction.md 4.1 was about CreateFileA decoding its narrow
// argument through the process's active code page, so a directory outside
// that code page could not be opened at all -- which lands on a class of
// user defined by the spelling of their name. CreateFileW fixed it, and the
// unit test that came with it exercises the UTF-8 to UTF-16 conversion in
// isolation. This is the other half, and the half the finding was actually
// about: a real segment, created under a real non-ASCII directory, written
// to, and read back.
//
// It runs everywhere. On POSIX it passes trivially -- UTF-8 bytes go
// through to open() untouched -- and that is worth having anyway, because
// it pins the contract that Logger::Options::directory_ and
// Logger::segmentPath() are UTF-8 on every platform rather than "whatever
// std::string means here".

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <filesystem>
#include <string>

namespace {

constexpr sub0log::SubsystemId cStorage{3};

} // namespace

TEST_CASE("a segment under a non-ASCII directory round-trips (R8.1)")
{
    // Latin-1 range and well outside it: the first is what a European user
    // name looks like, the second is what breaks an assumption that
    // "non-ASCII" means "one byte with the high bit set".
    const std::string directory = sub0log::test::freshUtf8Directory("utf8dir", "café-日本語");

    std::string segmentPath;
    {
        auto logger = sub0log::Logger::create({.directory_ = directory});
        REQUIRE(logger.valid());
        REQUIRE(logger.error().what_.empty());
        sub0log::Logger::ScopedBind bind{logger};

        sub0log_info(cStorage, "wrote {} bytes", std::uint64_t{4096});
        sub0log_error(cStorage, "and one error");

        segmentPath = logger.segmentPath();
        CHECK(logger.stats().droppedRecords_ == 0u);
    }

    // The path the library reports is UTF-8, so it goes back through the
    // UTF-8 door rather than std::filesystem::path{std::string}.
    const auto onDisk = sub0log::test::pathFromUtf8(segmentPath);
    REQUIRE(std::filesystem::exists(onDisk));
    CHECK(segmentPath.find("café") != std::string::npos);

    const auto image = sub0log::test::slurp(onDisk);
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    CHECK(decoder.undecodableRecords() == 0u);
    CHECK(reader.unreadableBytes() == 0u);
    REQUIRE(records.size() == 2u);
    CHECK(records[0].site_->format_ == "wrote {} bytes");
    CHECK(records[1].site_->format_ == "and one error");

    std::filesystem::remove_all(sub0log::test::pathFromUtf8(directory));
}

TEST_CASE("a directory that does not exist fails classified, not silently")
{
    // The neighbouring failure mode, and the one a bad path most often
    // produces: R9.2 says an unusable Logger must say so rather than
    // pretend, and a call site bound to it drops rather than crashes.
    const std::string directory =
        sub0log::test::freshUtf8Directory("utf8gone", "café") + "/no-such-subdirectory";

    auto logger = sub0log::Logger::create({.directory_ = directory});
    CHECK_FALSE(logger.valid());
    CHECK_FALSE(logger.error().what_.empty());

    sub0log::Logger::ScopedBind bind{logger};
    sub0log_info(cStorage, "into an invalid Logger");
    CHECK(logger.stats().droppedRecords_ == 1u);
}
