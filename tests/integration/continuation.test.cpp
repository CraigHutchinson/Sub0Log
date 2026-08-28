// Continuation chains: a Bytes argument longer than the inline cap is
// carried whole instead of cut at 512 bytes.
//
// docs/record-model.md names both the mechanism and the case it exists for:
// "Continuation records, for the medium case. A payload that does not fit
// one record spills into a bounded number of further records in the same
// thread's chunk... File paths are what this exists for."
//
// Two of these tests exist because the first implementation failed them.
// The chunk-exhaustion case caught a silent loss: a chain that reserved
// space and then found it could not finish left uncommitted head words
// behind, and SegmentReader::visit stops at the first one -- so a record
// committed *after* the abandoned space, and every record after that in the
// chunk, disappeared while being counted as unwritten rather than as
// damage. And the mixed fixed/bytes case caught a read of an argument
// length that was never written.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"

#include "support/test_framework.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cStorage{3};

/// The ceiling one argument can reach: the inline cap plus a full chain.
constexpr std::size_t cCeiling =
    static_cast<std::size_t>(sub0log::wire::cInlineBytesCap)
    * (1u + static_cast<std::size_t>(sub0log::wire::cMaxContinuations));

[[nodiscard]] std::string pattern(const std::size_t length)
{
    // Position-dependent bytes, so a slice reassembled out of order or off
    // by one shows up as a mismatch rather than passing on a run of 'x'.
    std::string value(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        value[i] = static_cast<char>('a' + (i % 26));
    }
    return value;
}

struct Decoded {
    std::vector<sub0log::DecodedRecord> records_;
    std::uint64_t undecodable_{};
    std::uint64_t unreadable_{};
    std::uint64_t skipped_{};
};

/// Decoding keeps the Decoder alive alongside the records, because a
/// reassembled argument is a view into storage the Decoder owns (reader.hpp
/// documents the borrow contract) -- so the caller must not outlive it.
template <typename Body>
void withDecoded(const std::filesystem::path& directory, Body&& body)
{
    const auto image = sub0log::test::slurp(sub0log::test::onlySegmentIn(directory));
    auto reader = sub0log::SegmentReader::open(image);
    REQUIRE(reader.valid());
    sub0log::Decoder decoder;
    Decoded out;
    out.records_ = decoder.decodeAll(reader);
    out.undecodable_ = decoder.undecodableRecords();
    out.skipped_ = decoder.skippedRecords();
    out.unreadable_ = reader.unreadableBytes();
    body(out);
}

} // namespace

TEST_CASE("a Bytes argument round-trips whole on both sides of the inline cap")
{
    struct Case { const char* name_; std::size_t length_; bool expectChain_; };
    const Case cases[] = {
        {"one under the cap", sub0log::wire::cInlineBytesCap - 1u, false},
        {"exactly the cap", sub0log::wire::cInlineBytesCap, false},
        {"one over the cap", sub0log::wire::cInlineBytesCap + 1u, true},
        {"several continuations", 3000u, true},
        {"exactly the ceiling", cCeiling, true},
    };

    for (const Case& testCase : cases) {
        CAPTURE(testCase.name_);
        CAPTURE(testCase.length_);
        const auto directory = sub0log::test::freshDirectory("cont");
        const std::string value = pattern(testCase.length_);

        {
            auto logger = sub0log::Logger::create({.directory_ = directory.string()});
            REQUIRE(logger.valid());
            sub0log::Logger::ScopedBind bind{logger};
            sub0log_info(cStorage, "path {}", std::string_view{value});
            CHECK(logger.stats().droppedRecords_ == 0u);
            // Nothing was cut, so nothing may claim it was (R9.2 cuts both
            // ways: a truncation flag on an untruncated record is a lie).
            CHECK(logger.stats().truncatedRecords_ == 0u);
        }

        withDecoded(directory, [&](const Decoded& out) {
            CHECK(out.undecodable_ == 0u);
            CHECK(out.unreadable_ == 0u);
            // Continuations are consumed by reassembly, never left over as
            // a shape this layer has no use for.
            CHECK(out.skipped_ == 0u);
            REQUIRE(out.records_.size() == 1u);
            CHECK_FALSE(out.records_[0].truncated_);
            REQUIRE(out.records_[0].args_.size() == 1u);
            CHECK(std::get<std::string_view>(out.records_[0].args_[0]) == value);
        });

        std::filesystem::remove_all(directory);
    }
}

TEST_CASE("past the ceiling the cut is still visible, never silent")
{
    const auto directory = sub0log::test::freshDirectory("contcap");
    const std::string value = pattern(cCeiling + 777u);

    {
        auto logger = sub0log::Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cStorage, "path {}", std::string_view{value});
        CHECK(logger.stats().droppedRecords_ == 0u);
        CHECK(logger.stats().truncatedRecords_ == 1u);
    }

    withDecoded(directory, [&](const Decoded& out) {
        CHECK(out.undecodable_ == 0u);
        CHECK(out.unreadable_ == 0u);
        REQUIRE(out.records_.size() == 1u);
        CHECK(out.records_[0].truncated_);
        const auto arg = std::get<std::string_view>(out.records_[0].args_[0]);
        CHECK(arg.size() == cCeiling);
        // What survived is the *front* of the value, unshuffled.
        CHECK(arg == std::string_view{value}.substr(0, cCeiling));
    });

    std::filesystem::remove_all(directory);
}

TEST_CASE("long and short arguments mix in one record without disturbing each other")
{
    const auto directory = sub0log::test::freshDirectory("contmix");
    const std::string first = pattern(1500u);
    const std::string second = pattern(900u);

    {
        auto logger = sub0log::Logger::create({.directory_ = directory.string()});
        REQUIRE(logger.valid());
        sub0log::Logger::ScopedBind bind{logger};
        // A fixed argument on either side of two chained ones: the fixed
        // arguments must keep their own lengths, which is exactly what an
        // earlier version got wrong by never writing them.
        sub0log_info(cStorage, "{} {} {} {}", std::uint64_t{4242},
                     std::string_view{first}, std::string_view{second}, std::int32_t{-7});
        CHECK(logger.stats().droppedRecords_ == 0u);
    }

    withDecoded(directory, [&](const Decoded& out) {
        CHECK(out.undecodable_ == 0u);
        CHECK(out.unreadable_ == 0u);
        REQUIRE(out.records_.size() == 1u);
        REQUIRE(out.records_[0].args_.size() == 4u);
        CHECK(std::get<std::uint64_t>(out.records_[0].args_[0]) == 4242u);
        CHECK(std::get<std::string_view>(out.records_[0].args_[1]) == first);
        CHECK(std::get<std::string_view>(out.records_[0].args_[2]) == second);
        // Signed integers arrive widened: the decoder's variant carries
        // one signed and one unsigned 64-bit alternative rather than one
        // per width, so an int32_t argument reads back as int64_t.
        CHECK(std::get<std::int64_t>(out.records_[0].args_[3]) == -7);
    });

    std::filesystem::remove_all(directory);
}

TEST_CASE("a chunk too small for a chain loses bytes, never records")
{
    // The regression this file exists for. The chunk is deliberately too
    // small to hold the whole chain, so the producer must fall back to the
    // flat cap -- and, whatever it does, the ledger must balance and the
    // records around it must still be there. A reader that stops early
    // reports the rest of the chunk as *unwritten*, which is why asserting
    // on the record count matters more than asserting on damage: the
    // failure this catches was silent by construction.
    const auto directory = sub0log::test::freshDirectory("contsmall");

    sub0log::Logger::Options options{};
    options.directory_ = directory.string();
    options.segment_.segmentBytes_ = sub0log::wire::cSegmentHeaderBytes + (8u * 1024u);
    options.segment_.chunkBytes_ = 1024u; // smaller than a full chain needs

    constexpr int cBefore = 3;
    constexpr int cAfter = 3;
    const std::string value = pattern(3000u);

    {
        auto logger = sub0log::Logger::create(options);
        REQUIRE(logger.valid());
        sub0log::Logger::ScopedBind bind{logger};

        for (int i = 0; i < cBefore; ++i) {
            sub0log_info(cStorage, "before {}", static_cast<std::uint64_t>(i));
        }
        sub0log_info(cStorage, "path {}", std::string_view{value});
        for (int i = 0; i < cAfter; ++i) {
            sub0log_info(cStorage, "after {}", static_cast<std::uint64_t>(i));
        }

        const auto stats = logger.stats();
        withDecoded(directory, [&](const Decoded& out) {
            CHECK(out.unreadable_ == 0u); // nothing damaged, whatever else happened
            CHECK(out.undecodable_ == 0u);

            // Every record either decoded or was counted as dropped. This
            // is the assertion the silent-loss bug failed: it lost the
            // records after the abandoned reservation without counting one.
            const std::size_t emitted = cBefore + 1u + cAfter;
            CHECK(out.records_.size() + stats.droppedRecords_ == emitted);

            // And specifically: the records written after the oversized one
            // are still readable.
            std::size_t afterSeen = 0;
            for (const auto& record : out.records_) {
                if (record.site_ != nullptr && record.site_->format_ == "after {}") {
                    ++afterSeen;
                }
            }
            CHECK(afterSeen + stats.droppedRecords_ >= static_cast<std::size_t>(cAfter));
        });
    }

    std::filesystem::remove_all(directory);
}
