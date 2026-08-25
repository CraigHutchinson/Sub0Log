// 07_live_tail.cpp -- a miniature console tailer: the console view is a
// view.
//
// What this teaches: a reader needs nothing from the producer beyond the
// bytes it has already committed to the segment file. There is no API call
// here that asks the Logger "is anything new?", no shared queue, no
// callback registered on the producer path (R5.6's own writeup is explicit
// that a callback there would reintroduce exactly the unbounded work R1
// forbids -- the reader side simply never gets one). This example's
// "tailer" on the main thread is just: open the same file the producer
// still has mapped, decode everything, print whatever is past the count it
// printed last time, and repeat. That loop is the entire mechanism behind
// a `tail -f`-style console view of a process that is still running, and
// it would work exactly the same way from a completely different process
// (tests/integration/producer.test.cpp states this property directly: "a
// segment is readable while its producer still holds it open").
//
// Requirements demonstrated: the read side of R3 (a committed record is
// visible immediately, not after some flush) and the general shape R5.2's
// merge builds on, in the single-process case.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#endif

namespace {

constexpr sub0log::SubsystemId cWorker{1};
constexpr int cRecordsToWrite = 6;

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        return {};
    }
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

// The "producer": logs slowly, as if it were doing real work between each
// record, for a little under a second. Runs on its own thread so the main
// thread is free to act as the tailer below -- Logger::active() is a
// process-wide pointer (instance.hpp), not thread-local, so both threads
// see the one Logger bound by main() before this thread was even started.
void producerThread()
{
    for (int i = 0; i < cRecordsToWrite; ++i) {
        sub0log_info(cWorker, "processed item {}", i);
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
    }
}

} // namespace

int main()
{
    const auto dir = makeScratchDir("livetail");

    // A smaller segment than the library default keeps each re-read of the
    // "whole file so far" cheap enough to poll several times a second; it
    // changes nothing about the property being demonstrated.
    auto logger = sub0log::Logger::create({
        .directory_ = dir.string(),
        .stem_ = "livetail",
        .segment_ = {.segmentBytes_ = 256u * 1024u, .chunkBytes_ = 16u * 1024u},
    });
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    sub0log::Logger::ScopedBind bind{logger};
    std::thread producer{producerThread};

    std::size_t alreadyPrinted = 0;
    std::filesystem::path segmentPath;

    std::printf("-- tailing %s while its producer is still writing --\n", dir.string().c_str());
    while (alreadyPrinted < static_cast<std::size_t>(cRecordsToWrite)) {
        if (segmentPath.empty()) {
            for (const auto& entry : std::filesystem::directory_iterator{dir}) {
                if (entry.path().extension() == ".s0l") {
                    segmentPath = entry.path();
                }
            }
        }
        if (!segmentPath.empty()) {
            // A fresh image and a fresh Decoder every pass, deliberately.
            // DecodedSite::format_/file_ and a Bytes argument's
            // string_view all point *into* the image buffer that produced
            // them (reader.hpp: "Bytes/string views point into the segment
            // image") -- so a Decoder built from one pass's image is only
            // valid to read from as long as that exact buffer is still
            // alive. Keeping yesterday's Decoder and feeding it today's
            // image would silently leave its already-known sites pointing
            // at a buffer this loop is about to discard (decodeAll() never
            // overwrites a site it already has). Re-parsing from byte zero
            // every pass costs nothing at this scale and sidesteps the
            // question entirely, at the cost of the work being O(records
            // seen so far) per pass rather than O(new records) -- a real
            // tailer reading a large, long-lived segment would instead keep
            // one image buffer (and one Decoder) alive for its own
            // lifetime and only grow it, never replace it out from under
            // itself.
            const auto image = slurp(segmentPath);
            auto reader = sub0log::SegmentReader::open(image);
            if (reader.valid()) {
                sub0log::Decoder decoder;
                const auto records = decoder.decodeAll(reader);
                for (std::size_t i = alreadyPrinted; i < records.size(); ++i) {
                    std::printf("[new] %s\n", sub0log::Decoder::format(records[i]).c_str());
                }
                alreadyPrinted = records.size();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{80});
    }

    producer.join();

    std::printf("-- producer finished; tailer saw all %zu record(s) as they were written --\n", alreadyPrinted);

    std::filesystem::remove_all(dir);
    if (alreadyPrinted != static_cast<std::size_t>(cRecordsToWrite)) {
        std::fprintf(stderr, "expected %d records, tailed %zu\n", cRecordsToWrite, alreadyPrinted);
        return 1;
    }
    return 0;
}
