// 03_correlation.cpp -- correlation is a field, joined by equality.
//
// What this teaches: CorrelationScope (context.hpp) stamps a correlation id
// into every record emitted while it is active, on whichever thread is
// active -- the id lives in a thread_local, so nesting one scope inside
// another (a worker thread doing its own sub-steps) just works. Joining the
// records that belong to one activity afterwards is nothing more than
// grouping on DecodedRecord::correlationId_ -- an equality test, not a
// heuristic over timestamps or wording (R6.2). The three worker threads
// below interleave their writes into the same segment; the grouping below
// puts each activity's records back together regardless of that
// interleaving.
//
// Requirements demonstrated: R6.1 (automatic, cross-thread propagation),
// R6.2 (joining is equality on a field).

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
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

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path onlySegment(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l") {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

// One "activity": a nested CorrelationScope entered on a worker thread, so
// its records join the activity that spawned the thread (R6.1) while also
// carrying their own, more specific, id for records logged only within this
// step. Both ids are legitimate join keys; which one you group by depends
// on the question you are asking.
void doStep(const int stepNumber)
{
    sub0log::CorrelationScope step{};
    sub0log_info(cWorker, "step {} started", stepNumber);
    sub0log_debug(cWorker, "step {} did some work", stepNumber);
    sub0log_info(cWorker, "step {} finished", stepNumber);
}

} // namespace

int main()
{
    const auto dir = makeScratchDir("correlation");

    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "correlation"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    std::uint64_t activityId = 0;
    {
        sub0log::Logger::ScopedBind bind{logger};

        // One correlation scope on the main thread: this is "the activity".
        // Everything logged anywhere -- this thread or the three spawned
        // below -- while it is active carries this id, automatically
        // (R6.1). No id is passed as a function parameter anywhere in this
        // file.
        sub0log::CorrelationScope activity{};
        activityId = activity.id();
        sub0log_info(cWorker, "activity {} beginning, dispatching 3 steps", activityId);

        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back(doStep, i);
        }
        for (auto& t : threads) {
            t.join();
        }

        sub0log_info(cWorker, "activity {} complete", activityId);
    }

    const auto image = slurp(onlySegment(dir));
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open\n");
        return 1;
    }
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    std::printf("-- raw file order (however the 3 threads interleaved) --\n");
    for (const auto& record : records) {
        std::printf("correlation=%-20llu %s\n",
                    static_cast<unsigned long long>(record.correlationId_),
                    sub0log::Decoder::format(record).c_str());
    }

    // Grouping by correlationId_ is a plain equality-keyed map -- nothing
    // "log-shaped" about it. Whichever thread's step scope produced a
    // given id, every record carrying that id belongs to the same step
    // (R6.2).
    std::map<std::uint64_t, std::vector<const sub0log::DecodedRecord*>> byCorrelation;
    for (const auto& record : records) {
        byCorrelation[record.correlationId_].push_back(&record);
    }

    std::printf("\n-- grouped by correlation id (an equality join, not a heuristic) --\n");
    for (const auto& [id, group] : byCorrelation) {
        const char* label = (id == activityId) ? " (the top-level activity)" : " (one step)";
        std::printf("correlation=%llu%s, %zu record(s):\n",
                    static_cast<unsigned long long>(id), label, group.size());
        for (const auto* record : group) {
            std::printf("    %s\n", sub0log::Decoder::format(*record).c_str());
        }
    }

    // 2 activity-level records + 3 steps * 3 records each.
    if (records.size() != 11) {
        std::fprintf(stderr, "expected 11 records, decoded %zu\n", records.size());
        return 1;
    }
    // The activity id plus 3 distinct step ids: 4 groups.
    if (byCorrelation.size() != 4) {
        std::fprintf(stderr, "expected 4 correlation groups, found %zu\n", byCorrelation.size());
        return 1;
    }
    if (byCorrelation.at(activityId).size() != 2) {
        std::fprintf(stderr, "expected 2 records at the activity level\n");
        return 1;
    }

    std::filesystem::remove_all(dir);
    return 0;
}
