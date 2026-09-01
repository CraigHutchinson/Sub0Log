// Two independent readers tailing one live segment at once -- a gap the
// existing ladder never states directly. roundtrip.test.cpp's "readable
// while its producer still holds it open" proves reader-vs-writer; this
// proves reader-vs-reader, which turns out to need nothing extra: a reader
// never mmaps the live file at all (SegmentReader::open takes an in-memory
// image; every caller gets there by an ordinary read-only file read --
// examples/07_live_tail.cpp, tools/sub0log_cat.cpp), so two readers have no
// shared state to race on in the first place. This test exists to say that
// with evidence rather than by inspection: two reader threads, each with
// its own SegmentReader/Decoder, poll the same growing segment while a
// third thread produces into it, and neither interferes with the other or
// with the producer.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace {
constexpr sub0log::SubsystemId cWorker{7};
constexpr int cRecordsToWrite = 200;
} // namespace

TEST_CASE("two independent readers tailing one live segment do not interfere")
{
    const auto directory = sub0log::test::freshDirectory("concurrent-readers");

    auto logger = sub0log::Logger::create({.directory_ = directory.string()});
    REQUIRE(logger.valid());

    std::atomic<bool> stopReaders{false};
    std::atomic<std::size_t> maxSeenByReaderA{0};
    std::atomic<std::size_t> maxSeenByReaderB{0};

    // Each reader is a fresh SegmentReader/Decoder every pass -- the same
    // shape as 07_live_tail.cpp and sub0log-cat --follow -- reading whatever
    // bytes an ordinary ifstream slurp returns right now, independent of
    // whatever the other reader thread or the producer thread is doing.
    const auto tailOnce = [&directory]() -> std::size_t {
        const auto segmentPath = sub0log::test::onlySegmentIn(directory);
        if (segmentPath.empty()) {
            return 0;
        }
        const auto image = sub0log::test::slurp(segmentPath);
        auto reader = sub0log::SegmentReader::open(image);
        if (!reader.valid()) {
            return 0;
        }
        sub0log::Decoder decoder;
        const auto records = decoder.decodeAll(reader);
        CHECK(reader.unreadableBytes() == 0); // never torn/damaged mid-read
        CHECK(decoder.undecodableRecords() == 0);
        return records.size();
    };

    std::thread readerA{[&] {
        while (!stopReaders.load(std::memory_order_relaxed)) {
            maxSeenByReaderA.store(tailOnce(), std::memory_order_relaxed);
        }
        maxSeenByReaderA.store(tailOnce(), std::memory_order_relaxed); // one last pass
    }};
    std::thread readerB{[&] {
        while (!stopReaders.load(std::memory_order_relaxed)) {
            maxSeenByReaderB.store(tailOnce(), std::memory_order_relaxed);
        }
        maxSeenByReaderB.store(tailOnce(), std::memory_order_relaxed);
    }};

    {
        sub0log::Logger::ScopedBind bind{logger};
        for (int i = 0; i < cRecordsToWrite; ++i) {
            sub0log_info(cWorker, "record {}", i);
        }
    }

    stopReaders.store(true, std::memory_order_relaxed);
    readerA.join();
    readerB.join();

    CHECK(logger.stats().droppedRecords_ == 0);
    CHECK(maxSeenByReaderA.load() == static_cast<std::size_t>(cRecordsToWrite));
    CHECK(maxSeenByReaderB.load() == static_cast<std::size_t>(cRecordsToWrite));
}
