// 05_multi_process.cpp -- several processes, one merged, ordered stream.
//
// What this teaches: each process writes its own segment file and nothing
// is ever shared for writing between them -- no cross-process lock, no
// collector daemon, no process able to corrupt another's stream (R5.1).
// Ordering the resulting files into one timeline is entirely a read-time
// concern: sub0log::Merger reads every segment's anchor pair (a monotonic
// reading paired with a wall-clock one, written once before any record) and
// uses it to align readings from processes whose steady_clock epochs are
// not portably comparable (R5.3) -- producers never coordinate about time
// at all.
//
// The correlation id set on the parent crosses into the child through
// SUB0LOG_CORRELATION in its environment -- the one mechanism that survives
// exec() of an unmodified binary (R5.4) -- so the merged stream below can
// show a child's work joined to the activity that spawned it by nothing
// more than comparing that field (R6.2).
//
// POSIX only: fork() is POSIX; spawning a real subprocess on Windows is
// CreateProcess and belongs with 04/06's Windows note, not duplicated here.
//
// Requirements demonstrated: R5.1-R5.4.

#ifndef _WIN32

#include <sub0log/context.hpp>
#include <sub0log/log.hpp>
#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp> // Decoder::format(), used only for the printout below

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cWorker{1};

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

} // namespace

int main()
{
    const auto dir = makeScratchDir("multiprocess");

    auto parentLogger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "parent"});
    if (!parentLogger.valid()) {
        std::fprintf(stderr, "could not create the parent Logger\n");
        return 1;
    }

    std::uint64_t activityId = 0;
    {
        sub0log::Logger::ScopedBind bind{parentLogger};
        sub0log::CorrelationScope activity{};
        activityId = activity.id();

        sub0log_info(cWorker, "parent: dispatching work to a child, activity={}", activityId);

        const pid_t child = ::fork();
        if (child < 0) {
            std::fprintf(stderr, "fork failed\n");
            return 1;
        }
        if (child == 0) {
            // The child: an unmodified-binary story. `activityId` reached
            // this process only because fork() copied the parent's memory
            // wholesale -- that copy is not what R5.4 is about. What R5.4
            // is about is this next line: the id is placed into the
            // process's *environment*, the one channel that would still
            // carry it across an execvp() of a real third-party binary that
            // shares none of this process's memory. Sub0Log::create() below
            // reads it back with exactly the same call a genuinely exec'd,
            // cooperating binary would make -- there is no fork-specific
            // shortcut in the library side of this story.
            ::setenv("SUB0LOG_CORRELATION", std::to_string(activityId).c_str(), 1);

            // Its own Logger, over the same directory but a different stem,
            // so it writes its own file and never touches the parent's
            // (R5.1).
            auto childLogger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "child"});
            if (!childLogger.valid()) { ::_exit(2); }
            sub0log::Logger::ScopedBind childBind{childLogger};

            sub0log::CorrelationScope inherited{sub0log::detail::correlationFromEnvironment()};
            sub0log_info(cWorker, "child: picked up activity {} from the environment", inherited.id());
            sub0log_info(cWorker, "child: doing the work the parent dispatched");
            sub0log_info(cWorker, "child: done");
            ::_exit(0);
        }

        int status = 0;
        ::waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::fprintf(stderr, "child did not exit cleanly\n");
            return 1;
        }

        sub0log_info(cWorker, "parent: child finished, wrapping up");
    }

    // --- read-time: merge both segments into one ordered stream ---------
    std::vector<std::vector<std::byte>> images;
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l") {
            images.push_back(slurp(entry.path()));
        }
    }
    if (images.size() != 2) {
        std::fprintf(stderr, "expected 2 segment files (parent + child), found %zu\n", images.size());
        return 1;
    }

    sub0log::Merger merger;
    for (const auto& image : images) {
        if (merger.addSegment(image) != sub0log::SegmentError::Ok) {
            std::fprintf(stderr, "a segment failed to open\n");
            return 1;
        }
    }
    const auto merged = merger.merged();

    std::printf("-- merged stream, ordered by anchored time (R5.2/R5.3) --\n");
    for (const auto& entry : merged) {
        std::printf("process=%-12llu correlation=%-20llu %s\n",
                    static_cast<unsigned long long>(entry.processId_),
                    static_cast<unsigned long long>(entry.record_.correlationId_),
                    sub0log::Decoder::format(entry.record_).c_str());
    }

    if (merged.size() != 5) {
        std::fprintf(stderr, "expected 5 merged records, found %zu\n", merged.size());
        return 1;
    }
    // The parent's records bookend the child's in this program's logical
    // order; the merge should have reproduced exactly that, from nothing
    // but each segment's own anchor pair and no coordination between the
    // two processes while they ran.
    const bool orderLooksRight =
        sub0log::Decoder::format(merged[0].record_).find("dispatching") != std::string::npos &&
        sub0log::Decoder::format(merged[4].record_).find("wrapping up") != std::string::npos;
    if (!orderLooksRight) {
        std::fprintf(stderr, "merged order did not match the expected parent/child interleaving\n");
        return 1;
    }

    // Every child record carries the parent's activity id -- an equality
    // test on correlationId_, nothing more (R6.2) -- and a distinct
    // processId_ from the parent's own records (R5.1: separate segments).
    std::size_t childRecords = 0;
    for (const auto& entry : merged) {
        if (entry.record_.correlationId_ == activityId && entry.processId_ != merged[0].processId_) {
            ++childRecords;
        }
    }
    if (childRecords != 3) {
        std::fprintf(stderr, "expected 3 child records joined by correlation id, found %zu\n", childRecords);
        return 1;
    }

    std::filesystem::remove_all(dir);
    return 0;
}

#else // _WIN32

#include <cstdio>

int main()
{
    // fork()+exec() is a POSIX story; CreateProcess is the Windows
    // equivalent and, like the rest of the process-spawning examples, is a
    // v2 item (docs/architecture.md's phasing). Skipping here.
    std::puts("05_multi_process: POSIX-only demo (fork); this is a v2 story on Windows. Skipping.");
    return 0;
}

#endif // !_WIN32
