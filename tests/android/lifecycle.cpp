// Android pause/resume/termination, established by a test rather than
// assumed from the desktop guarantee (issue #2). A NativeActivity with no
// Java code; tests/android/run_lifecycle.sh drives it on an emulator and
// decodes what it left behind with the desktop sub0log-cat.
//
// It logs the same records to two places at once, because the question is
// what each backend keeps when Android kills the process:
//
//   - a file segment in app-private storage (Logger::create over
//     internalDataPath): a shared mapping, so a record is in the kernel's
//     page cache the moment its commit store completes (R3.1);
//   - an in-memory segment (Logger::createInMemory over a static buffer),
//     written out whole on every pause -- the "hand off at pause" recipe
//     (examples/13_rotation.cpp) -- because a paused app may be killed
//     without any further callback.
//
// Records: "created", every RESUME/PAUSE/STOP/DESTROY callback, and a
// heartbeat tick whenever the looper is idle for 100 ms. Every pause record
// carries both Loggers' drop counters, so the checker can rule out "the
// buffer was full" as the reason anything is missing.
//
// Sizing, and why it is generous: a thread holds one cached chunk, for one
// Logger at a time (Logger::currentWriter). Alternating two Loggers on one
// thread, as both() does, therefore claims a fresh chunk in each segment on
// every switch -- one chunk per record. Hence small chunks and many of them:
// 1 KiB chunks in the 8 MiB file segment (~8000), 256-byte chunks in a
// 1 MiB buffer (~4000), against ~10 ticks a second. The driver then
// force-stops the app (SIGKILL, what the system does to a background app
// under memory pressure) and checks what survived.

#include <sub0log/log.hpp>

#include <android/log.h>
#include <android/looper.h>
#include <android_native_app_glue.h>

#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr sub0log::SubsystemId cApp{1};

alignas(8) std::byte gMemory[1024u * 1024u];

struct State {
    sub0log::Logger* file = nullptr;
    sub0log::Logger* memory = nullptr;
    std::string dir;
    unsigned run = 0;
    unsigned resumes = 0;
    unsigned pauses = 0;
    std::uint64_t tick = 0;
};

/// One call site, recorded into both segments in turn.
template <typename Emit>
void both(State& state, Emit&& emit)
{
    {
        sub0log::Logger::ScopedBind bind{*state.file};
        emit();
    }
    {
        sub0log::Logger::ScopedBind bind{*state.memory};
        emit();
    }
}

/// Which launch of the app this is, persisted across process deaths so the
/// driver can tell run 1's records from run 2's.
unsigned nextRun(const std::string& dir)
{
    const std::string path = dir + "/runs.txt";
    unsigned run = 0;
    if (std::FILE* in = std::fopen(path.c_str(), "r")) {
        if (std::fscanf(in, "%u", &run) != 1) {
            run = 0;
        }
        std::fclose(in);
    }
    ++run;
    if (std::FILE* out = std::fopen(path.c_str(), "w")) {
        std::fprintf(out, "%u\n", run);
        std::fclose(out);
    }
    return run;
}

/// The hand-off: the in-memory segment, whole, to a file of its own.
void dumpMemory(const State& state)
{
    const std::string path = state.dir + "/memory-run" + std::to_string(state.run) + "-pause"
                           + std::to_string(state.pauses) + ".s0l";
    if (std::FILE* out = std::fopen(path.c_str(), "wb")) {
        std::fwrite(gMemory, 1, sizeof(gMemory), out);
        std::fclose(out);
    }
}

void onAppCmd(android_app* const app, const std::int32_t cmd)
{
    State& state = *static_cast<State*>(app->userData);
    switch (cmd) {
    case APP_CMD_RESUME:
        ++state.resumes;
        both(state, [&] {
            sub0log_info(cApp, "resume run {} n {} tick {}", state.run, state.resumes, state.tick);
        });
        break;
    case APP_CMD_PAUSE:
        ++state.pauses;
        both(state, [&] {
            sub0log_info(cApp, "pause run {} n {} tick {} dropped file {} memory {}", state.run,
                         state.pauses, state.tick, state.file->stats().droppedRecords_,
                         state.memory->stats().droppedRecords_);
        });
        dumpMemory(state);
        break;
    case APP_CMD_STOP:
        both(state, [&] { sub0log_info(cApp, "stop run {} tick {}", state.run, state.tick); });
        break;
    case APP_CMD_DESTROY:
        both(state, [&] { sub0log_info(cApp, "destroy run {} tick {}", state.run, state.tick); });
        break;
    default:
        break;
    }
}

} // namespace

void android_main(android_app* const app)
{
    State state;
    if (app->activity->internalDataPath == nullptr || *app->activity->internalDataPath == '\0') {
        // Nothing to write to; say so where the driver's logcat dump shows it.
        __android_log_print(ANDROID_LOG_INFO, "sub0log", "no internalDataPath: nothing logged");
        return;
    }
    state.dir = app->activity->internalDataPath;
    ::mkdir(state.dir.c_str(), 0700);
    state.run = nextRun(state.dir);

    sub0log::Logger::Options fileOptions{};
    fileOptions.directory_ = state.dir;
    fileOptions.stem_ = "lifecycle";
    fileOptions.segment_.chunkBytes_ = 1024u;
    auto file = sub0log::Logger::create(fileOptions);
    sub0log::Logger::Options memoryOptions{};
    memoryOptions.segment_.chunkBytes_ = 256u;
    auto memory = sub0log::Logger::createInMemory(gMemory, memoryOptions);
    __android_log_print(ANDROID_LOG_INFO, "sub0log", "run %u: file valid=%d (%s) memory valid=%d",
                        state.run, file.valid() ? 1 : 0, file.segmentPath().c_str(),
                        memory.valid() ? 1 : 0);
    state.file = &file;
    state.memory = &memory;

    both(state, [&] { sub0log_info(cApp, "created run {}", state.run); });

    app->userData = &state;
    app->onAppCmd = onAppCmd;

    while (app->destroyRequested == 0) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int result =
            ALooper_pollOnce(100, nullptr, &events, reinterpret_cast<void**>(&source));
        if (result >= 0 && source != nullptr) {
            source->process(app, source);
        } else if (result == ALOOPER_POLL_TIMEOUT) {
            ++state.tick;
            both(state, [&] { sub0log_info(cApp, "tick run {} {}", state.run, state.tick); });
        }
    }
    both(state, [&] { sub0log_info(cApp, "exit run {} tick {}", state.run, state.tick); });
    app->userData = nullptr;
}
