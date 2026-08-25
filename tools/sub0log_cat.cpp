// sub0log-cat -- print a segment.
//
// Every consumer of this library was writing the same forty lines before
// they could see anything at all: open the file, SegmentReader::open,
// decodeAll, loop, print. The decoder has always been able to do this
// (Decoder::format renders a record); what was missing was the thing you
// run. REQUIREMENTS.md listed "whether the decoder ships as a library, a
// CLI, or both" as an open question and the examples answered it by each
// growing their own printer.
//
// It is deliberately a reader and nothing else: no configuration, no
// daemon, no index. Point it at a file or a directory, get records in time
// order across every process that wrote one.

#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/version.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::string_view cSegmentExtension = ".s0l";

struct Options {
    std::vector<std::filesystem::path> inputs_{};
    sub0log::Severity minimum_{sub0log::Severity::Trace};
    std::vector<std::uint32_t> subsystems_{};
    std::uint64_t correlation_{0};
    bool follow_{false};
    bool stats_{false};
    bool helpOnly_{false};
};

[[nodiscard]] const char* severityName(const sub0log::Severity severity) noexcept
{
    switch (severity) {
    case sub0log::Severity::Trace:        return "TRACE";
    case sub0log::Severity::Debug:        return "DEBUG";
    case sub0log::Severity::Info:         return "INFO ";
    case sub0log::Severity::Warning:      return "WARN ";
    case sub0log::Severity::Error:        return "ERROR";
    case sub0log::Severity::Unclassified: return "UNCLS";
    case sub0log::Severity::Fatal:        return "FATAL";
    }
    return "?????";
}

[[nodiscard]] bool parseSeverity(const std::string_view name, sub0log::Severity& out) noexcept
{
    struct Entry { std::string_view name_; sub0log::Severity value_; };
    static constexpr Entry cTable[] = {
        {"trace", sub0log::Severity::Trace},
        {"debug", sub0log::Severity::Debug},
        {"info", sub0log::Severity::Info},
        {"warning", sub0log::Severity::Warning},
        {"warn", sub0log::Severity::Warning},
        {"error", sub0log::Severity::Error},
        {"unclassified", sub0log::Severity::Unclassified},
        {"fatal", sub0log::Severity::Fatal},
    };
    for (const Entry& entry : cTable) {
        if (entry.name_ == name) {
            out = entry.value_;
            return true;
        }
    }
    return false;
}

/// Wall-clock text for a merged record. alignedNs_ is already
/// anchorWall + (mono - anchorMono) -- the arithmetic that puts two
/// processes' readings on one timeline (R5.3) -- so it is nanoseconds since
/// the Unix epoch and needs no further adjustment here.
[[nodiscard]] std::string wallTimeText(const std::uint64_t alignedNs)
{
    const auto seconds = static_cast<std::time_t>(alignedNs / 1'000'000'000ull);
    const auto nanos = static_cast<unsigned>(alignedNs % 1'000'000'000ull);

    std::tm parts{};
#if defined(_WIN32)
    if (::gmtime_s(&parts, &seconds) != 0) {
        return "<bad-time>";
    }
#else
    if (::gmtime_r(&seconds, &parts) == nullptr) {
        return "<bad-time>";
    }
#endif

    char buffer[40];
    const int written = std::snprintf(buffer, sizeof(buffer),
                                      "%04d-%02d-%02dT%02d:%02d:%02d.%09uZ",
                                      parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                                      parts.tm_hour, parts.tm_min, parts.tm_sec, nanos);
    return written > 0 ? std::string{buffer, static_cast<std::size_t>(written)}
                       : std::string{"<bad-time>"};
}

[[nodiscard]] std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    return bytes;
}

/// Expands each input into segment files: a file is itself, a directory is
/// every *.s0l directly inside it, sorted so a run is reproducible.
[[nodiscard]] std::vector<std::filesystem::path> collectSegments(const Options& options)
{
    std::vector<std::filesystem::path> paths;
    for (const auto& input : options.inputs_) {
        std::error_code ec;
        if (std::filesystem::is_directory(input, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator{input, ec}) {
                if (entry.path().extension() == cSegmentExtension) {
                    paths.push_back(entry.path());
                }
            }
        } else {
            paths.push_back(input);
        }
    }
    std::ranges::sort(paths);
    return paths;
}

[[nodiscard]] bool wanted(const Options& options, const sub0log::DecodedRecord& record) noexcept
{
    if (record.site_ == nullptr) {
        return false;
    }
    if (!sub0log::atLeast(record.site_->severity_, options.minimum_)) {
        return false;
    }
    if (options.correlation_ != 0 && record.correlationId_ != options.correlation_) {
        return false;
    }
    if (!options.subsystems_.empty()
        && std::ranges::find(options.subsystems_, record.site_->subsystem_.value_)
               == options.subsystems_.end()) {
        return false;
    }
    return true;
}

void printUsage()
{
    std::printf(
        "sub0log-cat %.*s -- print Sub0Log segments in time order\n"
        "\n"
        "Usage: sub0log-cat [options] <file-or-directory>...\n"
        "\n"
        "  -f, --follow             keep reading as producers write\n"
        "  -l, --level <name>       only this severity or above\n"
        "                           (trace|debug|info|warning|error|unclassified|fatal)\n"
        "  -s, --subsystem <id>     only this subsystem id (repeatable)\n"
        "  -c, --correlation <id>   only this correlation id\n"
        "      --stats              report the mechanism's own counters when done\n"
        "  -h, --help               this text\n"
        "\n"
        "A directory argument means every *.s0l file directly inside it. Several\n"
        "segments are merged onto one timeline, which is how a group of processes\n"
        "is meant to be read (docs/multi-process.md). Subsystems print by name\n"
        "when the segment declared one and by number otherwise; --subsystem always\n"
        "takes the number, which is what a record actually carries.\n",
        static_cast<int>(sub0log::libraryVersion().size()), sub0log::libraryVersion().data());
}

[[nodiscard]] bool parseArguments(const int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sub0log-cat: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };

        if (argument == "-h" || argument == "--help") {
            printUsage();
            options.helpOnly_ = true;
            return true;
        }
        if (argument == "-f" || argument == "--follow") {
            options.follow_ = true;
        } else if (argument == "--stats") {
            options.stats_ = true;
        } else if (argument == "-l" || argument == "--level") {
            const char* const value = next("--level");
            if (value == nullptr || !parseSeverity(value, options.minimum_)) {
                std::fprintf(stderr, "sub0log-cat: unknown severity '%s'\n",
                             value != nullptr ? value : "");
                return false;
            }
        } else if (argument == "-s" || argument == "--subsystem") {
            const char* const value = next("--subsystem");
            if (value == nullptr) {
                return false;
            }
            options.subsystems_.push_back(
                static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)));
        } else if (argument == "-c" || argument == "--correlation") {
            const char* const value = next("--correlation");
            if (value == nullptr) {
                return false;
            }
            options.correlation_ = std::strtoull(value, nullptr, 10);
        } else if (argument.starts_with('-') && argument.size() > 1) {
            std::fprintf(stderr, "sub0log-cat: unknown option '%s'\n", argv[i]);
            return false;
        } else {
            options.inputs_.emplace_back(argument);
        }
    }

    if (options.inputs_.empty()) {
        std::fprintf(stderr, "sub0log-cat: no input given (try --help)\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!parseArguments(argc, argv, options)) {
        return 2;
    }
    if (options.helpOnly_) {
        return 0;
    }

    // In --follow the whole set is re-read and re-merged each pass, and only
    // records past the previous count are printed. Re-reading is what makes
    // a tailer correct here rather than clever: a producer commits with a
    // release store into a mapping the reader can already see, so there is
    // no notification to subscribe to and nothing to keep open (R3.1). The
    // assumption the count relies on is that a later pass only appends to
    // the merged order, which holds while producers' clocks move forward.
    std::size_t alreadyPrinted = 0;
    bool anySegmentOpened = false;

    for (;;) {
        const auto paths = collectSegments(options);

        // The images must outlive the Merger: it hands out string_views into
        // them (reader.hpp's borrow contract), so they are held here for the
        // whole pass rather than inside the loop that adds them.
        std::vector<std::vector<std::byte>> images;
        images.reserve(paths.size());
        sub0log::Merger merger;
        for (const auto& path : paths) {
            images.push_back(slurp(path));
            if (images.back().empty()) {
                continue;
            }
            const sub0log::SegmentError error = merger.addSegment(images.back());
            if (error != sub0log::SegmentError::Ok) {
                std::fprintf(stderr, "sub0log-cat: %s is not a readable segment\n",
                             path.string().c_str());
                continue;
            }
            anySegmentOpened = true;
        }

        // Merger::subsystemName searches every segment it was given, so
        // asking it once per record would be quadratic in a merge of many
        // segments. The answer cannot change within a pass, so it is asked
        // once per id.
        std::unordered_map<std::uint32_t, std::string> subsystemLabels;
        const auto labelFor = [&](const sub0log::SubsystemId subsystem) -> const std::string& {
            const auto found = subsystemLabels.find(subsystem.value_);
            if (found != subsystemLabels.end()) {
                return found->second;
            }
            const std::string_view name = merger.subsystemName(subsystem);
            std::string label = std::to_string(subsystem.value_);
            if (!name.empty()) {
                // Name and number both: the name is what a person reads, the
                // number is what --subsystem takes and what the record
                // actually carries.
                label = std::string{name} + "(" + label + ")";
            }
            return subsystemLabels.emplace(subsystem.value_, std::move(label)).first->second;
        };

        const auto merged = merger.merged();
        for (std::size_t i = alreadyPrinted; i < merged.size(); ++i) {
            const sub0log::MergedRecord& entry = merged[i];
            if (!wanted(options, entry.record_)) {
                continue;
            }
            std::printf("%s  pid=%llu tid=%llu  %s  %s  %s%s\n",
                        wallTimeText(entry.alignedNs_).c_str(),
                        static_cast<unsigned long long>(entry.processId_),
                        static_cast<unsigned long long>(entry.record_.ownerThread_),
                        severityName(entry.record_.site_->severity_),
                        labelFor(entry.record_.site_->subsystem_).c_str(),
                        sub0log::Decoder::format(entry.record_).c_str(),
                        entry.record_.truncated_ ? " [truncated]" : "");
        }
        alreadyPrinted = merged.size();

        if (!options.follow_) {
            if (options.stats_) {
                // After the records, not interleaved with them: the counters
                // go to stderr so a pipeline keeps working, and stdout is
                // block-buffered when redirected, so without this the
                // summary overtakes what it is summarising.
                std::fflush(stdout);
                const sub0log::Merger::Totals totals = merger.totals();
                std::fprintf(stderr,
                             "-- %zu record(s); unreadable %llu byte(s), unwritten %llu byte(s), "
                             "undecodable %llu record(s)\n",
                             merged.size(),
                             static_cast<unsigned long long>(totals.unreadableBytes_),
                             static_cast<unsigned long long>(totals.unwrittenBytes_),
                             static_cast<unsigned long long>(totals.undecodableRecords_));
            }
            break;
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    if (!anySegmentOpened) {
        std::fprintf(stderr, "sub0log-cat: nothing readable in the given path(s)\n");
        return 1;
    }
    return 0;
}
