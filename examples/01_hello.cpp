// 01_hello.cpp -- the five-minute example.
//
// What this teaches: the two halves of Sub0Log's promise in one file. The
// call sites below (sub0log_debug, sub0log_warning, ...) never format a
// string, never allocate, and never take a lock -- they just copy typed
// argument bytes into a memory-mapped file. All of the *text* you see on
// stdout is produced afterwards, by the reader half of the library, reading
// the file back. If you comment out the printing loop at the bottom, the
// program still runs to completion and still writes a complete segment file
// -- nothing about logging depends on anyone ever looking at it.
//
// Requirements demonstrated: R1 (no work on the producer path), R2.1 (typed
// values survive to the reader).

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#endif

namespace {

// A subsystem id is just a number the library treats as opaque (see
// severity.hpp: "the library must not own the vocabulary" -- that story is
// told properly in 02_subsystems.cpp). One is enough here.
constexpr sub0log::SubsystemId cApp{1};

// The library has no opinion on how a Severity should be printed -- that is
// presentation, and presentation is the consumer's job, same as the
// subsystem name table in 02_subsystems.cpp. A short switch is all it takes.
std::string_view severityName(const sub0log::Severity severity)
{
    switch (severity) {
    case sub0log::Severity::Trace:        return "TRACE";
    case sub0log::Severity::Debug:        return "DEBUG";
    case sub0log::Severity::Info:         return "INFO";
    case sub0log::Severity::Warning:      return "WARNING";
    case sub0log::Severity::Error:        return "ERROR";
    case sub0log::Severity::Unclassified: return "UNCLASSIFIED";
    case sub0log::Severity::Fatal:        return "FATAL";
    }
    return "?";
}

// Every example builds its own scratch directory and cleans up after itself
// -- ctest runs this unattended, so nothing may linger.
std::filesystem::path makeScratchDir(const char* const tag)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("sub0log-example-" + std::string{tag} + "-" + std::to_string(SUB0LOG_EXAMPLE_PID()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Reads the one .s0l file a Logger with this stem produced. A real reader
// (a tail tool, a merge tool) would glob for several; one process here
// writes exactly one segment.
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

} // namespace

int main()
{
    const auto dir = makeScratchDir("hello");

    // --- producer side -------------------------------------------------
    //
    // Logger::create maps a file; nothing is written to it yet beyond the
    // segment header. ScopedBind makes this the *active* instance for the
    // duration of its scope -- the call-site macros below reach it through
    // Logger::active(), a single process-wide pointer, not a parameter you
    // thread through your call stack.
    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "hello"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger in %s\n", dir.string().c_str());
        return 1;
    }

    {
        sub0log::Logger::ScopedBind bind{logger};

        // Four call sites, four severities, mixed argument types (a
        // uint64_t, an int32_t, a double, a string_view). None of this line
        // formats "42" into text, allocates a std::string for the path, or
        // blocks on anything -- it copies raw bytes into a chunk this
        // thread already owns. Try it: nothing here even requires
        // <cstdio> or <string> on the producer's include list.
        sub0log_trace(cApp, "starting up, pid={}", SUB0LOG_EXAMPLE_PID());
        sub0log_debug(cApp, "loaded config from {} ({} bytes)", std::string_view{"app.cfg"}, std::uint64_t{2048});
        sub0log_info(cApp, "listening on port {}", std::int32_t{8080});
        sub0log_warning(cApp, "cache hit rate {} is below target", 0.42);

        // bind goes out of scope here: the previous (null) instance is
        // restored. The mapping stays open until `logger` itself is
        // destroyed at the end of main -- unmapping is not what makes the
        // file durable; the earlier commits already are (R3.1).
    }

    // --- consumer side ---------------------------------------------------
    //
    // A completely separate concern from the four lines above: open the
    // file as bytes, hand it to SegmentReader, and let Decoder turn the raw
    // records back into typed values. This could be a different process,
    // minutes later, on a machine that never linked this binary -- the file
    // is the whole interface (docs/architecture.md).
    const auto segmentPath = onlySegment(dir);
    if (segmentPath.empty()) {
        std::fprintf(stderr, "no segment file found in %s\n", dir.string().c_str());
        return 1;
    }
    const auto image = slurp(segmentPath);

    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open (SegmentError=%d)\n", static_cast<int>(reader.error()));
        return 1;
    }

    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    // Two different numbers, and the difference matters. "unwritten" is the
    // part of the pre-sized segment the producer never reached -- expected,
    // and normally most of the file. "damage" is what could not be
    // interpreted: a torn record, a bad length, storage from an older run.
    // On a healthy segment damage is zero however little of it was used.
    std::printf("read %s\n", segmentPath.filename().string().c_str());
    std::printf("decoded %zu record(s); %llu byte(s) never written, "
                "%llu byte(s) damaged, %llu undecodable record(s)\n\n",
                records.size(),
                static_cast<unsigned long long>(reader.unwrittenBytes()),
                static_cast<unsigned long long>(reader.unreadableBytes()),
                static_cast<unsigned long long>(decoder.undecodableRecords()));

    // format() is the one place text gets made, and only because we asked
    // for it here. Everything before this point in the program -- the four
    // call sites, the file, the decode pass -- worked without a single
    // std::string of rendered output existing anywhere.
    for (const auto& record : records) {
        std::printf("[%-11s] %s\n", severityName(record.site_->severity_).data(),
                    sub0log::Decoder::format(record).c_str());
    }

    if (records.size() != 4) {
        std::fprintf(stderr, "expected 4 records, decoded %zu\n", records.size());
        return 1;
    }

    std::filesystem::remove_all(dir);
    return 0;
}
