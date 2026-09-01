// 0_workedexample.cpp -- the flagship: a live terminal dashboard showing
// every producer shape this library supports writing into *one* segment at
// *the same time*.
//
// Every other example in this ladder isolates one capability so it can be
// taught in a few dozen lines. This one puts them next to each other on
// screen, live, because that is the situation the library actually ships
// into: a real process has several worker threads logging concurrently
// (03_correlation) *and* shells out to tools it did not write
// (06_child_capture), and an operator watching it wants one consolidated
// view, not one file per mechanism. What is on screen:
//
//   - a variable number of in-process thread producers, each logging a mix
//     of severities into one of a few declared subsystems (02_subsystems),
//     each unit of work wrapped in its own CorrelationScope (03_correlation);
//   - child-process producers, spawned on demand, each a short self-limiting
//     script captured via ChildProcess (06_child_capture) with a
//     LineInterceptor (R5.6) that suppresses a boilerplate line, harvests a
//     count out of another, and -- the part unique to this file -- re-emits
//     the kept lines as typed records of its own under an "External"
//     subsystem, so a third-party tool's chatter shows up in the same
//     per-subsystem breakdown as everything this process wrote itself;
//   - a reader thread doing exactly what 07_live_tail.cpp does: reopen the
//     segment, decode it fresh, repeat -- proof that the console view is a
//     view, not a special path;
//   - the operating counters 09_operating.cpp introduces one at a time, all
//     at once: Logger::stats() (dropped/truncated), unboundEmits(), and the
//     decoder's own undecodableRecords()/skippedRecords().
//
// **Why a producer is added, never removed, on the child-process side.**
// sub0log::ChildProcess has no terminate(): destroying one calls wait(),
// which blocks until the child actually exits (child.hpp's own class
// comment). There is deliberately no way to ask a running child to go away
// early. So a "spawn a child" keypress here does exactly that and nothing
// more -- the script it launches is bounded (a fixed, small number of
// iterations, see buildChildScript() below), runs to completion on its own
// joinable thread, and its wait() there is what drains and reaps it; the
// live "child producers running" count you see on screen falls back to zero
// on its own once every spawned script has finished, the same way a real
// short-lived tool would leave the process tree. Thread producers, by
// contrast, this file fully owns end to end: each carries its own
// std::atomic<bool> stop flag, and '-' signals and joins the most recently
// added one rather than waiting for it to decide to stop.
//
// **The ctest-safe half of this file.** Every other example in this ladder
// runs unattended to completion under `ctest -L example`; this one is
// interactive by nature, so it cannot be that example *and* honour that
// contract with the same code path. runSmoke() below is the second path:
// entered whenever stdout is not a real terminal (which is always true
// under ctest) or `--smoke` is passed explicitly, it runs the identical
// producer and reader machinery headless for a couple of seconds at a fixed,
// small scale, prints a plain-text summary, and returns 0 -- no FTXUI
// screen is ever constructed on that path. runInteractive() is the only
// place FTXUI is touched at all.
//
// Requirements demonstrated: R1.2/R2.1 (typed, no formatting at the call
// site), R2.2/R2.3 (consumer-owned subsystems), R5.5/R5.6 (child capture and
// interception), R6.1/R6.2 (cross-thread correlation), R9.1/R9.2/R9.3 (every
// counter this library exposes about its own health, together).

#include <sub0log/child.hpp>
#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#  define SUB0LOG_EXAMPLE_ISATTY(fd) (::_isatty(fd) != 0)
#  define SUB0LOG_EXAMPLE_FILENO(stream) ::_fileno(stream)
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#  define SUB0LOG_EXAMPLE_ISATTY(fd) (::isatty(fd) != 0)
#  define SUB0LOG_EXAMPLE_FILENO(stream) ::fileno(stream)
#endif

namespace {

// --- the application's own vocabulary (02_subsystems: this is ours to
// pick, not the library's) --------------------------------------------
constexpr sub0log::SubsystemId cStorage{1};
constexpr sub0log::SubsystemId cNetwork{2};
constexpr sub0log::SubsystemId cWorker{3};
// Nothing a raw ChildStart/ChildOutput/ChildExit record carries a
// subsystem_ field at all (child.hpp: those are attributed by childId_, not
// by subsystem) -- so this bucket is populated a different way, on purpose:
// the LineInterceptor below re-emits every kept child line as a typed
// sub0log_info() under cExternal from the capture thread itself. That is
// two records for the price of one captured line (the raw ChildOutput, and
// this typed one), and it is what makes "External" a real, comparable row
// in the per-subsystem breakdown rather than a bucket that can only ever
// read zero.
constexpr sub0log::SubsystemId cExternal{4};

constexpr std::array<std::pair<sub0log::SubsystemId, std::string_view>, 4> cSubsystemNames{{
    {cStorage, "Storage"},
    {cNetwork, "Network"},
    {cWorker, "Worker"},
    {cExternal, "External"},
}};
// Thread producers rotate through the three in-process subsystems; External
// is reserved for the interceptor path above.
constexpr std::array<sub0log::SubsystemId, 3> cThreadSubsystems{cStorage, cNetwork, cWorker};

constexpr std::size_t cTailSize = 18; // "most recent ~15-20 decoded records"

// --- small helpers shared with the rest of the ladder ------------------

std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::byte> slurp(const std::string& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        return {};
    }
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

// argv for "run this one-line shell script and exit", chosen per platform
// the same way examples/06_child_capture.cpp's shellArgv() does.
std::vector<std::string> shellArgv(const std::string& script)
{
#if defined(_WIN32)
    return {"cmd.exe", "/c", script};
#else
    return {"sh", "-c", script};
#endif
}

// A self-limiting "third-party tool": a fixed, small number of ticks, then
// it exits on its own -- see the file comment above for why that bound is
// the whole story on how a running child producer ever goes away. One line
// is deliberately named "boilerplate" (suppressed by the interceptor
// below, R5.6) and the closing line carries a count the interceptor
// harvests back out (also R5.6). The Windows arm's per-tick delay is a
// `ping` round trip rather than a real sleep -- cmd.exe has no builtin for
// one -- so its timing is approximate; nothing here depends on it being
// exact, only bounded.
std::string buildChildScript(const std::uint64_t index, const int iterations)
{
    const std::string tag = "child " + std::to_string(index);
    std::string script;
#if defined(_WIN32)
    script = "echo " + tag + " starting up boilerplate&";
    for (int i = 1; i <= iterations; ++i) {
        script += "echo " + tag + " tick " + std::to_string(i) + " of " + std::to_string(iterations) + "&";
        script += "ping -n 1 -w 60 127.0.0.1 >nul&";
    }
    script += "echo " + tag + " processed " + std::to_string(iterations) + " items, shutting down";
#else
    script = "echo " + tag + " starting up boilerplate; ";
    for (int i = 1; i <= iterations; ++i) {
        script += "echo " + tag + " tick " + std::to_string(i) + " of " + std::to_string(iterations) + "; ";
        script += "sleep 0.05; ";
    }
    script += "echo " + tag + " processed " + std::to_string(iterations) + " items, shutting down";
#endif
    return script;
}

std::string_view severityLabel(const sub0log::Severity severity)
{
    switch (severity) {
    case sub0log::Severity::Trace:        return "TRACE";
    case sub0log::Severity::Debug:        return "DEBUG";
    case sub0log::Severity::Info:         return "INFO";
    case sub0log::Severity::Warning:      return "WARN";
    case sub0log::Severity::Error:        return "ERROR";
    case sub0log::Severity::Unclassified: return "UNCLASS";
    case sub0log::Severity::Fatal:        return "FATAL";
    }
    return "?";
}

ftxui::Color severityColor(const sub0log::Severity severity)
{
    using ftxui::Color;
    switch (severity) {
    case sub0log::Severity::Trace:        return Color::GrayDark;
    case sub0log::Severity::Debug:        return Color::Cyan;
    case sub0log::Severity::Info:         return Color::Default;
    case sub0log::Severity::Warning:      return Color::Yellow;
    case sub0log::Severity::Error:        return Color::Red;
    case sub0log::Severity::Unclassified: return Color::Magenta;
    case sub0log::Severity::Fatal:        return Color::RedLight;
    }
    return Color::Default;
}

std::string formatRate(const double perSecond)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f/s", perSecond);
    return std::string{buf};
}

// ---------------------------------------------------------------------------
// The read side: exactly 07_live_tail.cpp's loop (slurp, SegmentReader::
// open, a *fresh* Decoder every pass -- see reader.hpp's "Lifetime, and the
// mistake it invites" for why re-using one across a re-read into a
// different buffer is the segfault this project already made once), except
// the result lands in a mutex-protected snapshot instead of stdout, because
// the FTXUI render callback and this loop run on different threads.

struct TailEntry {
    std::string text_{};
    sub0log::Severity severity_{sub0log::Severity::Info};
};

struct ReadSnapshot {
    std::uint64_t decodedTotal_{0};
    std::uint64_t undecodableRecords_{0};
    std::uint64_t skippedRecords_{0};
    std::array<std::uint64_t, cSubsystemNames.size()> perSubsystem_{};
    std::vector<TailEntry> tail_{};
    double recordsPerSecond_{0.0};
};

class ReaderState {
public:
    // Re-reads `segmentPath` from byte zero and republishes a fresh
    // snapshot. Safe to call from any one thread repeatedly (this example
    // only ever calls it from the dedicated reader thread); snapshot()
    // is safe to call from any thread at any time.
    void update(const std::string& segmentPath)
    {
        const auto image = slurp(segmentPath);
        if (image.empty()) {
            return; // nothing committed yet -- leave the previous snapshot.
        }
        auto reader = sub0log::SegmentReader::open(image);
        if (!reader.valid()) {
            return;
        }
        sub0log::Decoder decoder; // fresh every pass -- see the class comment above.
        const auto records = decoder.decodeAll(reader);

        ReadSnapshot snapshot;
        snapshot.decodedTotal_ = records.size();
        snapshot.undecodableRecords_ = decoder.undecodableRecords();
        snapshot.skippedRecords_ = decoder.skippedRecords();

        for (const auto& record : records) {
            if (record.site_ == nullptr) {
                continue;
            }
            for (std::size_t i = 0; i < cSubsystemNames.size(); ++i) {
                if (cSubsystemNames[i].first == record.site_->subsystem_) {
                    ++snapshot.perSubsystem_[i];
                    break;
                }
            }
        }

        const std::size_t tailStart = records.size() > cTailSize ? records.size() - cTailSize : 0;
        snapshot.tail_.reserve(records.size() - tailStart);
        for (std::size_t i = tailStart; i < records.size(); ++i) {
            const sub0log::Severity severity =
                records[i].site_ != nullptr ? records[i].site_->severity_ : sub0log::Severity::Info;
            snapshot.tail_.push_back(TailEntry{sub0log::Decoder::format(records[i]), severity});
        }

        const auto now = std::chrono::steady_clock::now();
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (havePrevious_) {
                const double dt = std::chrono::duration<double>(now - previousTime_).count();
                if (dt > 0.0 && snapshot.decodedTotal_ >= previousTotal_) {
                    snapshot.recordsPerSecond_ = static_cast<double>(snapshot.decodedTotal_ - previousTotal_) / dt;
                }
            }
            previousTime_ = now;
            previousTotal_ = snapshot.decodedTotal_;
            havePrevious_ = true;
            snapshot_ = std::move(snapshot);
        }
    }

    [[nodiscard]] ReadSnapshot snapshot() const
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        return snapshot_;
    }

private:
    mutable std::mutex mutex_{};
    ReadSnapshot snapshot_{};
    bool havePrevious_{false};
    std::chrono::steady_clock::time_point previousTime_{};
    std::uint64_t previousTotal_{0};
};

// ---------------------------------------------------------------------------
// The producer side.

// One in-process producer thread this file fully owns: heap-allocated (via
// unique_ptr in AppState::threadProducers_) so its address never moves out
// from under the running thread when the owning vector grows or shrinks --
// the same reason Decoder's own class comment gives for never holding more
// than one in a relocating container.
struct ThreadProducer {
    std::atomic<bool> stop_{false};
    std::thread thread_{};
    int id_{0};
    sub0log::SubsystemId subsystem_{};
};

// The subsystem is a template parameter, not a runtime field read out of
// `self`, for a reason specific to how the call-site macros work: each
// sub0log_*() expansion declares its own `static constinit SiteDescriptor`
// (log.hpp), which means the subsystem baked into it must be a compile-time
// constant at that call site -- exactly like the severity already is (every
// example in this ladder passes a `constexpr SubsystemId`, never a
// variable). A thread producer's subsystem is decided once, when it is
// created, so that decision is pushed to compile time via this template
// parameter instead, and addThreadProducer() below picks which
// instantiation to launch based on the runtime rotation.
template <sub0log::SubsystemId Subsystem>
void threadProducerLoop(ThreadProducer& self)
{
    std::mt19937 rng{std::random_device{}() ^ (static_cast<unsigned>(self.id_) * 2654435761u)};
    std::uniform_int_distribution<int> sleepMs{20, 150};
    std::uint64_t tick = 0;

    while (!self.stop_.load(std::memory_order_relaxed)) {
        // One CorrelationScope per simulated "unit of work" -- exactly
        // 03_correlation.cpp's doStep(), so records from every producer
        // thread, spread across an arbitrary number of threads a user adds
        // and removes live, still join back into activities by an equality
        // test on correlationId_ rather than by which thread wrote them.
        sub0log::CorrelationScope unit{};
        const double sample = static_cast<double>(tick % 97) * 1.5;
        switch (tick % 4) {
        case 0:
            sub0log_trace(Subsystem, "producer {} tick {} sample {}", self.id_, tick, sample);
            break;
        case 1:
            sub0log_debug(Subsystem, "producer {} tick {} sample {}", self.id_, tick, sample);
            break;
        case 2:
            sub0log_info(Subsystem, "producer {} tick {} sample {}", self.id_, tick, sample);
            break;
        default:
            sub0log_warning(Subsystem, "producer {} tick {} sample {}", self.id_, tick, sample);
            break;
        }
        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds{sleepMs(rng)});
    }
}

// Everything the dashboard (or the smoke path) needs to own across its
// lifetime: the live thread producers, the joinable child-producer threads
// (see the file comment above on why they are never killed, only waited
// on), and the counters the render callback reads.
struct AppState {
    std::vector<std::unique_ptr<ThreadProducer>> threadProducers_{};
    int nextProducerId_{0};
    std::vector<std::thread> childThreads_{};
    std::atomic<std::uint64_t> totalChildrenSpawned_{0};
    std::atomic<std::uint64_t> runningChildren_{0};
    std::atomic<std::uint64_t> harvestedItemsTotal_{0};
    ReaderState reader_{};
};

void addThreadProducer(AppState& state)
{
    auto producer = std::make_unique<ThreadProducer>();
    producer->id_ = state.nextProducerId_++;
    const std::size_t rotation = static_cast<std::size_t>(producer->id_) % cThreadSubsystems.size();
    producer->subsystem_ = cThreadSubsystems[rotation];
    ThreadProducer* const raw = producer.get();
    // The runtime rotation picks which *compile-time* instantiation of
    // threadProducerLoop<>() this thread runs -- see that template's own
    // comment for why the subsystem cannot simply be `self.subsystem_`.
    switch (rotation) {
    case 0:
        producer->thread_ = std::thread{[raw] { threadProducerLoop<cStorage>(*raw); }};
        break;
    case 1:
        producer->thread_ = std::thread{[raw] { threadProducerLoop<cNetwork>(*raw); }};
        break;
    default:
        producer->thread_ = std::thread{[raw] { threadProducerLoop<cWorker>(*raw); }};
        break;
    }
    state.threadProducers_.push_back(std::move(producer));
}

void removeThreadProducer(AppState& state)
{
    if (state.threadProducers_.empty()) {
        return;
    }
    ThreadProducer& victim = *state.threadProducers_.back();
    victim.stop_.store(true, std::memory_order_relaxed);
    if (victim.thread_.joinable()) {
        victim.thread_.join();
    }
    state.threadProducers_.pop_back();
}

// Spawns one bounded child-process producer, fire-and-forget: the thread
// this pushes onto childThreads_ owns the ChildProcess entirely, calls
// wait() on it (off the UI thread, so the dashboard never blocks on a
// child), and exits once the script does. Nothing here ever asks a running
// child to stop early -- see the file comment above for why that is not a
// capability this library, or this example, has.
void spawnChildProducer(AppState& state)
{
    const std::uint64_t index = state.totalChildrenSpawned_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    state.runningChildren_.fetch_add(1u, std::memory_order_relaxed);

    state.childThreads_.emplace_back([&state, index] {
#if defined(_WIN32)
        constexpr int cIterations = 8; // fewer: each tick's `ping` round trip is slower than a real sleep.
#else
        constexpr int cIterations = 30;
#endif
        const std::string script = buildChildScript(index, cIterations);

        sub0log::ChildOptions options{.argv_ = shellArgv(script)};
        // R5.6 on display twice over: a boilerplate line is suppressed
        // (counted, never silent), and every line that survives is both
        // captured as a raw ChildOutput record (the mechanism 06 teaches)
        // *and* re-logged here, on this same capture thread, as a typed
        // Message under cExternal -- two different, both honest, views of
        // the same child chatter. The final "processed N items" line also
        // has its count harvested into a shared counter, the same live-
        // value-out-of-a-streaming-child trick 06's own interceptor uses
        // for a port number.
        options.onLine_ = [&state](const sub0log::ChildLine& line) {
            if (line.text_.find("boilerplate") != std::string_view::npos) {
                return sub0log::InterceptAction::Suppress;
            }
            constexpr std::string_view marker = "processed ";
            const auto pos = line.text_.find(marker);
            if (pos != std::string_view::npos) {
                const auto rest = line.text_.substr(pos + marker.size());
                std::uint64_t harvested = 0;
                const auto result = std::from_chars(rest.data(), rest.data() + rest.size(), harvested);
                if (result.ec == std::errc{}) {
                    state.harvestedItemsTotal_.fetch_add(harvested, std::memory_order_relaxed);
                }
            }
            sub0log_info(cExternal, "external: {}", line.text_);
            return sub0log::InterceptAction::Log;
        };

        auto child = sub0log::ChildProcess::spawn(options);
        if (child.valid()) {
            (void)child.wait(); // blocks this thread only, never the UI thread.
        }
        state.runningChildren_.fetch_sub(1u, std::memory_order_relaxed);
    });
}

// Stops and joins every thread producer, then joins every child-producer
// thread (each is bounded, so this is a short wait at most -- see the file
// comment on why there is no faster way to end a running one). Shared by
// both the interactive and the smoke path so shutdown behaves identically.
void shutdown(AppState& state)
{
    for (auto& producer : state.threadProducers_) {
        producer->stop_.store(true, std::memory_order_relaxed);
    }
    for (auto& producer : state.threadProducers_) {
        if (producer->thread_.joinable()) {
            producer->thread_.join();
        }
    }
    state.threadProducers_.clear();

    for (auto& t : state.childThreads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    state.childThreads_.clear();
}

// ---------------------------------------------------------------------------
// The interactive dashboard.

ftxui::Element renderUi(const AppState& state, const sub0log::Logger& logger)
{
    using namespace ftxui;

    const ReadSnapshot snap = state.reader_.snapshot();
    const sub0log::Logger::Stats stats = logger.stats();

    auto statRow = [](const std::string& label, const std::string& value) {
        return hbox({text(label) | size(WIDTH, EQUAL, 26), text(value) | bold});
    };

    Elements statsRows{
        statRow("decoded records", std::to_string(snap.decodedTotal_)),
        statRow("rate (~1s window)", formatRate(snap.recordsPerSecond_)),
        statRow("dropped records", std::to_string(stats.droppedRecords_)),
        statRow("truncated records", std::to_string(stats.truncatedRecords_)),
        statRow("unbound emits", std::to_string(sub0log::unboundEmits())),
        statRow("undecodable records", std::to_string(snap.undecodableRecords_)),
        statRow("skipped (child/other)", std::to_string(snap.skippedRecords_)),
        separator(),
        statRow("thread producers", std::to_string(state.threadProducers_.size())),
        statRow("child producers spawned", std::to_string(state.totalChildrenSpawned_.load(std::memory_order_relaxed))),
        statRow("child producers running", std::to_string(state.runningChildren_.load(std::memory_order_relaxed))),
        statRow("harvested item count", std::to_string(state.harvestedItemsTotal_.load(std::memory_order_relaxed))),
    };

    Elements subsystemRows;
    for (std::size_t i = 0; i < cSubsystemNames.size(); ++i) {
        subsystemRows.push_back(hbox({
            text(std::string{cSubsystemNames[i].second}) | size(WIDTH, EQUAL, 10),
            text(std::to_string(snap.perSubsystem_[i])),
        }));
    }

    Elements tailLines;
    if (snap.tail_.empty()) {
        tailLines.push_back(text("(waiting for the first record...)") | dim);
    }
    for (const auto& entry : snap.tail_) {
        tailLines.push_back(hbox({
            text(std::string{severityLabel(entry.severity_)}) | color(severityColor(entry.severity_))
                | size(WIDTH, EQUAL, 8),
            text(entry.text_) | color(severityColor(entry.severity_)),
        }));
    }

    return vbox({
        text("Sub0Log -- live multi-producer dashboard") | bold,
        text("[+/=] add thread producer  [-] remove thread producer  "
             "[c] spawn a child-process producer  [q] quit")
            | dim,
        separator(),
        hbox({
            window(text("counters"), vbox(std::move(statsRows))) | size(WIDTH, EQUAL, 42),
            window(text("per-subsystem"), vbox(std::move(subsystemRows))) | size(WIDTH, EQUAL, 22),
        }),
        window(text("recent records"), vbox(std::move(tailLines))) | flex,
    });
}

int runInteractive(sub0log::Logger& logger)
{
    using namespace ftxui;

    AppState state;
    addThreadProducer(state); // starts feeling alive immediately, rather than an empty screen.
    addThreadProducer(state);

    auto screen = ScreenInteractive::Fullscreen();

    // The reader thread: identical loop to 07_live_tail.cpp's, just landing
    // in state.reader_ instead of stdout, and nudging the screen to redraw
    // (screen.Post(Event::Custom)) after every fresh snapshot -- the same
    // "background thread posts a custom event" pattern FTXUI's own
    // homescreen example uses to animate a UI driven by state a component
    // does not own.
    std::atomic<bool> readerStop{false};
    std::thread readerThread{[&] {
        while (!readerStop.load(std::memory_order_relaxed)) {
            state.reader_.update(logger.segmentPath());
            screen.Post(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }};

    auto renderer = Renderer([&] { return renderUi(state, logger); });
    renderer |= CatchEvent([&](const Event event) {
        if (event == Event::Character('q') || event == Event::Character('Q') || event == Event::Escape) {
            screen.Exit();
            return true;
        }
        if (event == Event::Character('+') || event == Event::Character('=')) {
            addThreadProducer(state);
            return true;
        }
        if (event == Event::Character('-') || event == Event::Character('_')) {
            removeThreadProducer(state);
            return true;
        }
        if (event == Event::Character('c') || event == Event::Character('C')) {
            spawnChildProducer(state);
            return true;
        }
        return false;
    });

    screen.Loop(renderer);

    readerStop.store(true, std::memory_order_relaxed);
    readerThread.join();
    shutdown(state);
    return 0;
}

// ---------------------------------------------------------------------------
// The ctest-safe path: no ScreenInteractive is ever constructed here. Same
// producer/reader machinery as the interactive path, at a small fixed
// scale, for a short fixed duration, then a plain-text summary and exit 0
// -- this is the path `ctest -L example` actually runs (see the file
// comment above for why the two paths cannot share one code path and still
// both be honest to what they are).
int runSmoke(sub0log::Logger& logger)
{
    std::printf("sub0log_example_0_workedexample: no controlling terminal (or --smoke) -- running headless\n");

    AppState state;
    constexpr int cSmokeThreadProducers = 2;
    for (int i = 0; i < cSmokeThreadProducers; ++i) {
        addThreadProducer(state);
    }
    spawnChildProducer(state);

    constexpr auto cSmokeDuration = std::chrono::milliseconds{1800};
    std::this_thread::sleep_for(cSmokeDuration);

    shutdown(state);
    state.reader_.update(logger.segmentPath()); // one final read, after every producer has stopped.

    const ReadSnapshot snap = state.reader_.snapshot();
    const sub0log::Logger::Stats stats = logger.stats();

    std::printf("-- sub0log_example_0_workedexample: smoke summary --\n");
    std::printf("decoded records:          %llu\n", static_cast<unsigned long long>(snap.decodedTotal_));
    std::printf("dropped records:          %llu\n", static_cast<unsigned long long>(stats.droppedRecords_));
    std::printf("truncated records:        %llu\n", static_cast<unsigned long long>(stats.truncatedRecords_));
    std::printf("unbound emits:            %llu\n", static_cast<unsigned long long>(sub0log::unboundEmits()));
    std::printf("undecodable records:      %llu\n", static_cast<unsigned long long>(snap.undecodableRecords_));
    std::printf("skipped (child/other):    %llu\n", static_cast<unsigned long long>(snap.skippedRecords_));
    std::printf("child producers spawned:  %llu\n",
                static_cast<unsigned long long>(state.totalChildrenSpawned_.load(std::memory_order_relaxed)));
    std::printf("harvested item count:     %llu\n",
                static_cast<unsigned long long>(state.harvestedItemsTotal_.load(std::memory_order_relaxed)));
    for (std::size_t i = 0; i < cSubsystemNames.size(); ++i) {
        std::printf("  %-9.*s %llu\n", static_cast<int>(cSubsystemNames[i].second.size()),
                    cSubsystemNames[i].second.data(), static_cast<unsigned long long>(snap.perSubsystem_[i]));
    }

    if (snap.decodedTotal_ == 0u) {
        std::fprintf(stderr, "expected at least one decoded record from a smoke run\n");
        return 1;
    }
    if (snap.undecodableRecords_ != 0u) {
        std::fprintf(stderr, "expected zero undecodable records from a healthy smoke run\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--smoke") {
            smoke = true;
        }
    }
    // ctest runs every example with no controlling terminal, which is
    // exactly the condition that must route here even without --smoke: the
    // FTXUI full-screen loop reading raw keyboard input would otherwise
    // either hang or misbehave against a pipe (see the file comment above).
    const bool interactiveTerminal = SUB0LOG_EXAMPLE_ISATTY(SUB0LOG_EXAMPLE_FILENO(stdout));

    const auto dir = makeScratchDir("workedexample");

    sub0log::Logger::Options options{};
    options.directory_ = dir.string();
    options.stem_ = "workedexample";
    options.subsystemNames_ = cSubsystemNames;
    options.threshold_ = sub0log::Severity::Trace; // every severity this dashboard emits is visible.

    auto logger = sub0log::Logger::create(options);
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        std::filesystem::remove_all(dir);
        return 1;
    }
    sub0log::Logger::ScopedBind bind{logger};

    const int result = (smoke || !interactiveTerminal) ? runSmoke(logger) : runInteractive(logger);

    std::filesystem::remove_all(dir);
    return result;
}
