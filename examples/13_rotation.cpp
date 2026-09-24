// 13_rotation.cpp -- a long-running session on bounded segments: rotate
// before a segment fills, hand the finished one off, keep a bounded number.
//
// A segment does not wrap (README, "Known limitations"): once its chunks are
// claimed, every later record is dropped and counted. A process that runs
// for days -- a service, or an app session that lives across pause and
// resume -- therefore needs *more segments over time*, not a bigger one. This
// is the recipe, built from primitives the library already has:
//
//   1. Watch Logger::usage(): chunks claimed out of the total. One relaxed
//      load; cheap enough to check on every iteration of a main loop.
//   2. Before the current segment fills, create the next Logger and bind it.
//      Every producer thread picks the new binding up on its own next emit.
//   3. Retire the old one, then *hand it off*: here, move the file into an
//      "outbox" directory, which is where an uploader, a crash reporter or
//      an adb pull would take it from. A retired segment is complete and
//      closed, so whatever reads it next never races a writer.
//   4. Bound the outbox: keep the newest N, delete the oldest. That is the
//      retention policy, and it is the caller's decision, not the library's
//      (REQUIREMENTS.md keeps rotation policy out of the library on purpose).
//   5. Read back with Merger over everything kept: one ordered stream.
//
// Two triggers, because an app session wants both: the watermark (the
// segment is nearly full) and a lifecycle event (the app is being paused, so
// hand off what exists now while the process is still alive -- a paused app
// may be killed without further notice; docs/embedded.md, Android).
//
// One honest limit. ScopedBind is scoped (LIFO), so swapping the binding is
// "unbind, bind next". Here the swap runs on the only producer thread, so no
// emit can fall in between. With several producer threads, an emit landing
// in that gap is dropped and counted by sub0log::unboundEmits() (R9.3) --
// visible, not silent -- and the retired Logger must not be destroyed until
// every producer has emitted once since the swap (docs/embedded.md, item 7).
// Firmware main loops and app loops are the single-producer case.
//
// Requirements demonstrated: R9.1 (drops counted: none here), R5.2 (merge
// across segments), issue #2's rotation/handoff recipe.

#include <sub0log/log.hpp>
#include <sub0log/merge.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cSession{1};

[[nodiscard]] std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](const char c) { return static_cast<std::byte>(c); });
    return bytes;
}

/// Owns "the current segment" and everything about moving past it.
class Rotator {
public:
    Rotator(std::filesystem::path live, std::filesystem::path outbox, std::size_t keep)
        : live_{std::move(live)}, outbox_{std::move(outbox)}, keep_{keep}
    {
        std::filesystem::create_directories(live_);
        std::filesystem::create_directories(outbox_);
        open();
    }

    /// Step 1: the watermark. One chunk of headroom: a single producer
    /// holds at most one chunk, so rotating with one left can never drop.
    void rotateIfNearlyFull()
    {
        const auto usage = current_->usage();
        if (usage.chunksClaimed_ + 1u >= usage.chunkCount_) {
            rotate("watermark");
        }
    }

    /// Steps 2-4. Also called directly on a lifecycle event (pause).
    void rotate(const char* const why)
    {
        const std::filesystem::path finished{current_->segmentPath()};
        dropped_ += current_->stats().droppedRecords_;

        // Unbind, then retire: the mapping is released when the Logger is
        // destroyed, so the file is complete and closed before hand-off.
        bind_.reset();
        current_.reset();
        handOff(finished);
        ++rotations_;
        std::printf("  rotated (%s): handed off %s\n", why, finished.filename().string().c_str());
        open();
    }

    /// Hands off the last one too, so everything ends up in the outbox.
    void close()
    {
        const std::filesystem::path finished{current_->segmentPath()};
        dropped_ += current_->stats().droppedRecords_;
        bind_.reset();
        current_.reset();
        handOff(finished);
    }

    [[nodiscard]] int rotations() const noexcept { return rotations_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }

private:
    void open()
    {
        sub0log::Logger::Options options{};
        options.directory_ = live_.string();
        options.stem_ = "session";
        // Deliberately small, so this example rotates a few dozen times in
        // well under a second. A real session sizes for its rate
        // (README, "Operating it") and rotates far less often.
        options.segment_.chunkBytes_ = 1024u;
        options.segment_.segmentBytes_ = sub0log::wire::cSegmentHeaderBytes + 8u * 1024u;
        current_ = std::make_unique<sub0log::Logger>(sub0log::Logger::create(options));
        bind_.emplace(*current_);
    }

    void handOff(const std::filesystem::path& finished)
    {
        std::error_code ec;
        std::filesystem::rename(finished, outbox_ / finished.filename(), ec);
        if (ec) {
            std::fprintf(stderr, "hand-off failed: %s\n", ec.message().c_str());
            return;
        }
        // Step 4: retention. Oldest first by name is not creation order
        // (names carry a random generation), so order by write time.
        std::vector<std::filesystem::directory_entry> kept;
        for (const auto& entry : std::filesystem::directory_iterator(outbox_)) {
            kept.push_back(entry);
        }
        std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) {
            return a.last_write_time() < b.last_write_time();
        });
        while (kept.size() > keep_) {
            std::filesystem::remove(kept.front().path(), ec);
            kept.erase(kept.begin());
            ++evicted_;
        }
    }

    std::filesystem::path live_;
    std::filesystem::path outbox_;
    std::size_t keep_;
    std::unique_ptr<sub0log::Logger> current_;
    std::optional<sub0log::Logger::ScopedBind> bind_;
    int rotations_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t evicted_ = 0;
};

} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "sub0log-example-13";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    // Keep everything for the read-back check below; a real app keeps a
    // handful and uploads the rest.
    constexpr std::size_t cKeep = 1000;
    constexpr std::uint64_t cEvents = 3000;

    std::uint64_t emitted = 0;
    int rotations = 0;
    std::uint64_t dropped = 0;
    std::uint64_t evicted = 0;
    {
        Rotator rotator{root / "live", root / "outbox", cKeep};
        std::printf("session: %llu events, rotating on watermark and on 'pause'\n",
                    static_cast<unsigned long long>(cEvents));
        for (std::uint64_t i = 0; i < cEvents; ++i) {
            sub0log_info(cSession, "session event {}", i);
            ++emitted;
            if (i == cEvents / 2) {
                // A lifecycle event: hand off what exists now, while the
                // process is certainly still alive to do it.
                rotator.rotate("pause");
            } else {
                rotator.rotateIfNearlyFull();
            }
        }
        rotator.close();
        rotations = rotator.rotations();
        dropped = rotator.dropped();
        evicted = rotator.evicted();
    }

    // Step 5: everything in the outbox, merged back into one stream.
    sub0log::Merger merger;
    std::vector<std::vector<std::byte>> images;
    for (const auto& entry : std::filesystem::directory_iterator(root / "outbox")) {
        images.push_back(slurp(entry.path()));
    }
    for (const auto& image : images) {
        if (merger.addSegment(image) != sub0log::SegmentError::Ok) {
            std::fprintf(stderr, "a handed-off segment did not open\n");
            return 1;
        }
    }
    const auto merged = merger.merged();

    // Within one process every segment's anchors come from the same clocks,
    // but separately sampled anchor pairs can misorder records that land
    // microseconds apart across a boundary (docs/embedded.md, "Ordering
    // across rotated segments"). So this checks the set, not the order: every
    // event is present exactly once.
    std::vector<int> seen(cEvents, 0);
    for (const auto& record : merged) {
        const auto value = std::get<std::uint64_t>(record.record_.args_[0]);
        if (value < cEvents) {
            ++seen[value];
        }
    }
    const bool complete = std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; });

    std::printf("rotations=%d segments=%zu evicted=%llu emitted=%llu merged=%zu dropped=%llu\n",
                rotations, images.size(), static_cast<unsigned long long>(evicted),
                static_cast<unsigned long long>(emitted), merged.size(),
                static_cast<unsigned long long>(dropped));
    std::filesystem::remove_all(root, ec);

    if (rotations < 2 || dropped != 0 || merged.size() != emitted || !complete) {
        std::fprintf(stderr, "rotation lost or duplicated records\n");
        return 1;
    }
    std::printf("every event survived rotation and hand-off exactly once\n");

    // Retention for real: the same session keeping only the newest 3. The
    // outbox must hold exactly 3 segments, the rest evicted oldest-first --
    // in an app, the ones already uploaded.
    std::size_t outboxAfterRetention = 0;
    std::uint64_t evictedWithRetention = 0;
    {
        Rotator rotator{root / "live", root / "outbox", 3};
        for (std::uint64_t i = 0; i < cEvents; ++i) {
            sub0log_info(cSession, "session event {}", i);
            rotator.rotateIfNearlyFull();
        }
        rotator.close();
        evictedWithRetention = rotator.evicted();
    }
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator(root / "outbox")) {
        ++outboxAfterRetention;
    }
    std::filesystem::remove_all(root, ec);
    std::printf("retention keep=3: outbox=%zu evicted=%llu\n", outboxAfterRetention,
                static_cast<unsigned long long>(evictedWithRetention));
    if (outboxAfterRetention != 3 || evictedWithRetention == 0) {
        std::fprintf(stderr, "retention did not bound the outbox\n");
        return 1;
    }
    return 0;
}
