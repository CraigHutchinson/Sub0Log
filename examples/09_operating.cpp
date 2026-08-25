// 09_operating.cpp -- running the thing in production: what to watch, and
// what to turn up when something is wrong.
//
// The other examples show what the library records. This one shows what the
// library says about *itself*, because a logger that is quietly not logging
// is worse than no logger at all -- an operator who believes they have
// diagnostics stops looking for them.
//
// Three mechanisms, each answering a question an operator actually asks:
//
//   "Am I losing records?"        Logger::stats() -- two monotonic counters
//                                 (R9.1). Poll them from a metrics thread;
//                                 never from a callback, which would put
//                                 your code on the emit path (R1).
//   "Is anything logging at all?" sub0log::unboundEmits() -- call sites
//                                 that reached no instance (R9.3). Nonzero
//                                 and climbing means somebody is emitting
//                                 into nowhere.
//   "Can I see more, just here?"  Logger::setThreshold(subsystem, severity)
//                                 -- one component turned up without the
//                                 traffic of every other one.
//
// Requirements demonstrated: R9.1, R9.3, R1.4.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cStorage{1};
constexpr sub0log::SubsystemId cNetwork{2};

[[nodiscard]] std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

/// What a metrics exporter would do on its own thread: read the counters,
/// publish them, and go back to sleep. Reading is a relaxed load of an
/// atomic, so it is safe and cheap from anywhere and never interferes with
/// a producer.
void publishGauges(const sub0log::Logger& logger)
{
    const sub0log::Stats stats = logger.stats();
    std::printf("  sub0log_dropped_records   %llu\n",
                static_cast<unsigned long long>(stats.droppedRecords_));
    std::printf("  sub0log_truncated_records %llu\n",
                static_cast<unsigned long long>(stats.truncatedRecords_));
    std::printf("  sub0log_unbound_emits     %llu\n",
                static_cast<unsigned long long>(sub0log::unboundEmits()));
}

} // namespace

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "sub0log-example-09";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    // --- 1. before anything is bound -------------------------------------
    //
    // This is the shape of a real bug: a call site running before the
    // process binds its Logger, or inside a shared library that got its own
    // copy of the active-instance pointer. Nothing is written, nothing
    // fails, and without R9.3 nothing anywhere would say so.
    const std::uint64_t unboundBefore = sub0log::unboundEmits();
    sub0log_error(cStorage, "this record does not exist");
    if (sub0log::unboundEmits() == unboundBefore) {
        std::fprintf(stderr, "expected the unbound emit to be counted\n");
        return 1;
    }
    std::printf("before binding: %llu emit(s) reached no instance\n",
                static_cast<unsigned long long>(sub0log::unboundEmits() - unboundBefore));

    // --- 2. a deliberately tiny segment ----------------------------------
    //
    // Sized so the drop counter has something to say. A real service sizes
    // this for its rate; when a segment fills, every later record is
    // dropped and counted, which is why the counter is the thing to alert
    // on rather than the file's existence.
    sub0log::Logger::Options options{};
    options.directory_ = directory.string();
    options.segment_.segmentBytes_ = sub0log::wire::cSegmentHeaderBytes + 4096u;
    options.segment_.chunkBytes_ = 4096u;
    options.threshold_ = sub0log::Severity::Info;

    auto logger = sub0log::Logger::create(options);
    if (!logger.valid()) {
        std::fprintf(stderr, "Logger::create failed: %s\n", logger.error().what_.data());
        return 1;
    }
    sub0log::Logger::ScopedBind bind{logger};

    // --- 3. turning one subsystem up -------------------------------------
    //
    // Storage is misbehaving, so storage gets Debug. Network keeps the
    // process-wide Info, so investigating one component does not invite the
    // other one's traffic -- which at tens of nanoseconds a record is the
    // difference between watching something and drowning it.
    logger.setThreshold(cStorage, sub0log::Severity::Debug);
    sub0log_debug(cStorage, "blob {} refcount {}", std::uint64_t{7}, std::int32_t{3});
    sub0log_debug(cNetwork, "this one is still below the bar");
    sub0log_info(cNetwork, "this one is not");

    // --- 4. filling it, on purpose ---------------------------------------
    for (int i = 0; i < 2000; ++i) {
        sub0log_info(cStorage, "filler {}", static_cast<std::uint64_t>(i));
    }

    std::printf("what a metrics scrape would publish:\n");
    publishGauges(logger);

    const sub0log::Stats stats = logger.stats();
    if (stats.droppedRecords_ == 0u) {
        std::fprintf(stderr, "expected a full segment to report drops\n");
        return 1;
    }

    // --- 5. and the ledger balances --------------------------------------
    //
    // The point of counting drops is that emitted == decoded + dropped. A
    // reader can check that arithmetic against the segment, which is what
    // makes "no records were lost silently" a claim rather than a hope.
    const auto image = slurp(logger.segmentPath());
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "SegmentReader::open failed\n");
        return 1;
    }
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    std::printf("decoded %zu records, dropped %llu, undecodable %llu\n",
                records.size(), static_cast<unsigned long long>(stats.droppedRecords_),
                static_cast<unsigned long long>(decoder.undecodableRecords()));
    if (decoder.undecodableRecords() != 0u) {
        std::fprintf(stderr, "a full segment must not produce undecodable records\n");
        return 1;
    }

    std::filesystem::remove_all(directory);
    std::printf("example 09 OK\n");
    return 0;
}
