// 02_subsystems.cpp -- the consumer owns the vocabulary.
//
// What this teaches: sub0log::SubsystemId is an opaque number
// (severity.hpp). The library ships no enum of subsystems, no name table,
// and no opinion about what "Storage" or "Network" means -- docs/
// record-model.md puts it plainly: "a logging library that defines the
// subsystem enumeration cannot be reused by anything that needs a different
// one". So the *application* below defines its own subsystem constants and
// its own id-to-name table, the decoder never needs to know either exists,
// and filtering happens on the numeric field the record actually carries --
// never by searching the rendered text for a subsystem's name (R2.2).
//
// Requirements demonstrated: R2.2 (filter by field, not text), R2.3 (assert
// on fields).

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::_getpid())
#else
#  include <unistd.h>
#  define SUB0LOG_EXAMPLE_PID() static_cast<unsigned long>(::getpid())
#endif

namespace {

// --- the application's own vocabulary --------------------------------
//
// Sub0Log knows nothing below this line until it reads a record's
// subsystem_ field back as a plain std::uint32_t. Renumbering these, adding
// a fifth one, or using an enum class of your own instead of these
// constants are all changes entirely inside this file.
constexpr sub0log::SubsystemId cStorage{1};
constexpr sub0log::SubsystemId cNetwork{2};
constexpr sub0log::SubsystemId cUi{3};

struct NamedSubsystem {
    sub0log::SubsystemId id_;
    const char* name_;
};
constexpr std::array<NamedSubsystem, 3> cSubsystemNames{{
    {cStorage, "Storage"},
    {cNetwork, "Network"},
    {cUi, "Ui"},
}};

const char* nameOf(const sub0log::SubsystemId id)
{
    for (const auto& entry : cSubsystemNames) {
        if (entry.id_ == id) {
            return entry.name_;
        }
    }
    return "Unknown";
}

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

} // namespace

int main()
{
    const auto dir = makeScratchDir("subsystems");

    // Declaring the names puts this table *into the segment*, as
    // SubsystemDefinition records written before anything else. It is
    // optional and it changes nothing above: the library still ships no
    // enumeration of subsystems and still has no opinion about what 1
    // means. What it buys is that a segment recovered from a customer
    // machine a year from now decodes to "Storage" rather than
    // "subsystem 1", without the header this file's table lives in.
    static constexpr std::array<std::pair<sub0log::SubsystemId, std::string_view>, 3>
        cDeclaredNames{{
            {cStorage, "Storage"},
            {cNetwork, "Network"},
            {cUi, "Ui"},
        }};

    sub0log::Logger::Options options{};
    options.directory_ = dir.string();
    options.stem_ = "subsystems";
    options.subsystemNames_ = cDeclaredNames;

    auto logger = sub0log::Logger::create(options);
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    {
        sub0log::Logger::ScopedBind bind{logger};

        // A spread across all three subsystems and several severities, so
        // the filter below has something real to select from.
        sub0log_debug(cStorage, "opened blob store at {}", std::string_view{"/var/data"});
        sub0log_info(cStorage, "read {} bytes", std::uint64_t{4096});
        sub0log_warning(cStorage, "disk usage at {}%", std::uint32_t{88});
        sub0log_error(cStorage, "write failed, retrying (attempt {})", std::uint32_t{2});

        sub0log_info(cNetwork, "accepted connection from {}", std::string_view{"10.0.0.4"});
        sub0log_debug(cNetwork, "sent {} bytes", std::uint64_t{512});

        sub0log_trace(cUi, "frame {} rendered", std::uint32_t{60});
        sub0log_warning(cUi, "layout recomputed {} times this frame", std::uint32_t{5});
    }

    const auto image = slurp(onlySegment(dir));
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open\n");
        return 1;
    }
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);

    std::printf("-- everything, subsystem names resolved by the consumer --\n");
    for (const auto& record : records) {
        std::printf("[%-7s] %-11s %s\n", nameOf(record.site_->subsystem_),
                    severityName(record.site_->severity_).data(),
                    sub0log::Decoder::format(record).c_str());
    }

    // The same names, resolved without nameOf() and without this file: the
    // decoder read them out of the segment, where create() declared them.
    // That is the difference between a table you have to keep and one the
    // recording carries -- and it is what `sub0log-cat` prints for a
    // segment nobody has the source of.
    std::printf("-- the same names, read back out of the segment itself --\n");
    for (const auto& entry : cDeclaredNames) {
        const std::string_view declared = decoder.subsystemName(entry.first);
        std::printf("   subsystem %u is \"%.*s\"%s\n", entry.first.value_,
                    static_cast<int>(declared.size()), declared.data(),
                    declared == entry.second ? "" : "  <- MISMATCH");
        if (declared != entry.second) {
            std::fprintf(stderr, "the segment did not carry the declared name\n");
            return 1;
        }
    }

    // The filter that matters: Storage records at Warning or above. This
    // is two field comparisons -- SubsystemId equality and the severity
    // ladder's atLeast() -- against values that arrived as an integer and
    // an enum, never a scan of any record's rendered text (R2.2). A grep
    // over formatted log lines could get the same *answer* here by luck,
    // but it would be matching characters, not filtering a field, and it
    // would silently miss a differently-worded message tomorrow.
    std::printf("\n-- filtered: Storage, Warning or above (fields, not text) --\n");
    std::size_t matched = 0;
    for (const auto& record : records) {
        const bool isStorage = record.site_->subsystem_ == cStorage;
        const bool severeEnough = sub0log::atLeast(record.site_->severity_, sub0log::Severity::Warning);
        if (isStorage && severeEnough) {
            std::printf("[%-7s] %-11s %s\n", nameOf(record.site_->subsystem_),
                        severityName(record.site_->severity_).data(),
                        sub0log::Decoder::format(record).c_str());
            ++matched;
        }
    }

    if (records.size() != 8) {
        std::fprintf(stderr, "expected 8 total records, decoded %zu\n", records.size());
        return 1;
    }
    if (matched != 2) {
        // "disk usage" (Warning) and "write failed" (Error): the two
        // Storage records at or above Warning.
        std::fprintf(stderr, "expected 2 Storage/Warning+ records, matched %zu\n", matched);
        return 1;
    }

    std::filesystem::remove_all(dir);
    return 0;
}
