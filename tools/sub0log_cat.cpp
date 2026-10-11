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

#include <sub0log/json.hpp>
#include <sub0log/merge.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/version.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view cSegmentExtension = ".s0l";

enum class OutputFormat { Text, Jsonl };

struct Options {
    std::vector<std::filesystem::path> inputs_{};
    sub0log::Severity minimum_{sub0log::Severity::Trace};
    std::vector<std::uint32_t> subsystems_{};
    std::optional<std::uint64_t> correlation_{};
    std::optional<std::uint32_t> waitMs_{};
    std::optional<std::pair<std::size_t, std::uint64_t>> argUint_{};
    std::uint64_t maxInputBytes_{128u * 1024u * 1024u};
    std::uint64_t maxOutputBytes_{64u * 1024u * 1024u};
    std::uint64_t maxRecords_{1'000'000};
    std::uint64_t maxFiles_{1024};
    bool follow_{false};
    bool stats_{false};
    bool healthJson_{false};
    bool helpOnly_{false};
    OutputFormat format_{OutputFormat::Text};
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

struct BudgetError : std::runtime_error { using std::runtime_error::runtime_error; };

volatile std::sig_atomic_t gCancelled = 0;
void cancel(const int signal) { gCancelled = signal; }

template<class T>
[[nodiscard]] bool parseUnsigned(const std::string_view text, T& value)
{
    if (text.empty()) { return false; }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] std::vector<std::byte> slurp(const std::filesystem::path& path,
                                         std::uint64_t& remaining)
{
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) { throw std::runtime_error{"cannot open " + path.string()}; }
    const auto end = in.tellg();
    if (end < 0) { throw std::runtime_error{"cannot size " + path.string()}; }
    const auto size = static_cast<std::uint64_t>(end);
    if (size > remaining || size > std::numeric_limits<std::size_t>::max()
        || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw BudgetError{"input byte budget exceeded"};
    }
    remaining -= size;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size != 0 && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error{"cannot read " + path.string()};
    }
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
            const std::filesystem::directory_iterator entries{input, ec};
            if (ec) { throw std::runtime_error{"cannot list " + input.string()}; }
            for (const auto& entry : entries) {
                if (entry.path().extension() == cSegmentExtension) {
                    if (paths.size() >= options.maxFiles_) { throw BudgetError{"file budget exceeded"}; }
                    paths.push_back(entry.path());
                }
            }
        } else {
            if (paths.size() >= options.maxFiles_) { throw BudgetError{"file budget exceeded"}; }
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
    if (options.correlation_.has_value() && record.correlationId_ != *options.correlation_) {
        return false;
    }
    if (!options.subsystems_.empty()
        && std::ranges::find(options.subsystems_, record.site_->subsystem_.value_)
               == options.subsystems_.end()) {
        return false;
    }
    if (options.argUint_) {
        const auto [index, value] = *options.argUint_;
        if (index >= record.args_.size()) { return false; }
        const auto code = record.site_->argTypes_[index];
        if (code != sub0log::wire::TypeCode::U8 && code != sub0log::wire::TypeCode::U16
            && code != sub0log::wire::TypeCode::U32 && code != sub0log::wire::TypeCode::U64) {
            return false;
        }
        const auto* actual = std::get_if<std::uint64_t>(&record.args_[index]);
        if (actual == nullptr || *actual != value) { return false; }
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
        "      --format <name>      text (default) or jsonl\n"
        "      --wait-ms <ms>       wait for first match, including existing history\n"
        "      --arg-uint <i:v>     match unsigned argument index i exactly to v\n"
        "      --max-files <n>      files per scan (default 1024)\n"
        "      --max-input-bytes <n> bytes per scan (default 134217728)\n"
        "      --max-records <n>    decoded per scan / retained cursor (1000000)\n"
        "      --max-output-bytes <n> total output bytes (default 67108864)\n"
        "      --health-json        final health JSON on stderr after diagnostics\n"
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
        } else if (argument == "--health-json") {
            options.healthJson_ = true;
        } else if (argument == "--stats") {
            options.stats_ = true;
        } else if (argument == "--format") {
            const char* const value = next("--format");
            if (value == nullptr) {
                return false;
            }
            const std::string_view name{value};
            if (name == "text") {
                options.format_ = OutputFormat::Text;
            } else if (name == "jsonl") {
                options.format_ = OutputFormat::Jsonl;
            } else {
                std::fprintf(stderr, "sub0log-cat: unknown format '%s' (use text or jsonl)\n", value);
                return false;
            }
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
            std::uint32_t id{};
            if (!parseUnsigned(std::string_view{value}, id)) {
                std::fprintf(stderr, "sub0log-cat: invalid subsystem id\n");
                return false;
            }
            options.subsystems_.push_back(id);
        } else if (argument == "-c" || argument == "--correlation") {
            const char* const value = next("--correlation");
            if (value == nullptr) {
                return false;
            }
            std::uint64_t id{};
            if (!parseUnsigned(std::string_view{value}, id)) {
                std::fprintf(stderr, "sub0log-cat: invalid correlation id\n");
                return false;
            }
            options.correlation_ = id;
        } else if (argument == "--wait-ms") {
            const char* value = next("--wait-ms");
            std::uint32_t duration{};
            if (value == nullptr || !parseUnsigned(std::string_view{value}, duration)) {
                std::fprintf(stderr, "sub0log-cat: invalid wait duration\n");
                return false;
            }
            options.waitMs_ = duration;
            options.follow_ = true;
        } else if (argument == "--arg-uint") {
            const char* value = next("--arg-uint");
            if (value == nullptr) { return false; }
            const std::string_view text{value};
            const auto colon = text.find(':');
            std::size_t index{};
            std::uint64_t number{};
            if (colon == text.npos || !parseUnsigned(text.substr(0, colon), index)
                || !parseUnsigned(text.substr(colon + 1), number)) {
                std::fprintf(stderr, "sub0log-cat: invalid unsigned argument predicate\n");
                return false;
            }
            options.argUint_ = std::pair{index, number};
        } else if (argument == "--max-files" || argument == "--max-input-bytes"
                   || argument == "--max-records" || argument == "--max-output-bytes") {
            const char* value = next(argv[i]);
            std::uint64_t limit{};
            if (value == nullptr || !parseUnsigned(std::string_view{value}, limit) || limit == 0) {
                std::fprintf(stderr, "sub0log-cat: invalid resource limit\n");
                return false;
            }
            if (argument == "--max-files") { options.maxFiles_ = limit; }
            if (argument == "--max-input-bytes") { options.maxInputBytes_ = limit; }
            if (argument == "--max-records") { options.maxRecords_ = limit; }
            if (argument == "--max-output-bytes") { options.maxOutputBytes_ = limit; }
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

struct Health {
    std::uint64_t scans_{};
    std::uint64_t records_{};
    std::uint64_t emitted_{};
    bool inputErrorSeen_{};
    sub0log::Merger::Totals totals_{};
};

int run(const Options& options, Health& health)
{
    // Append-only wire identity, independent of discovery order and timestamps.
    // Keep history for the whole invocation: evicting removed segments would
    // replay them on rename/reappearance. Capacity exhaustion is an error.
    std::set<std::array<std::uint64_t, 3>> seen;
    bool anySegmentOpened = false;
    std::uint64_t outputBytes = 0;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{options.waitMs_.value_or(0)};
    std::signal(SIGINT, cancel);
    std::signal(SIGTERM, cancel);

    try {
        for (;;) {
            if (gCancelled != 0) { return 128 + gCancelled; }
            const auto paths = collectSegments(options);
            std::uint64_t remainingBytes = options.maxInputBytes_;
            bool scanError = false;

            // The images must outlive the Merger: it hands out string_views into
            // them (reader.hpp's borrow contract), so they are held here for the
            // whole pass rather than inside the loop that adds them.
            std::vector<std::vector<std::byte>> images;
            images.reserve(paths.size());
            sub0log::Merger merger;
            for (const auto& path : paths) {
                try {
                    images.push_back(slurp(path, remainingBytes));
                } catch (const BudgetError&) { throw; }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "sub0log-cat: %s\n", error.what());
                    scanError = true;
                    continue;
                }

                const sub0log::SegmentError error = merger.addSegment(images.back());
                if (error != sub0log::SegmentError::Ok) {
                    std::fprintf(stderr, "sub0log-cat: %s is not a readable segment\n",
                                 path.string().c_str());
                    scanError = true;
                    continue;
                }
                anySegmentOpened = true;
            }

            // Merger::subsystemName searches every segment it was given, so
            // asking it once per record would be quadratic in a merge of many
            // segments. The answer cannot change within a pass, so it is asked
            // once per id.
            std::unordered_map<std::uint32_t, std::string> subsystemLabels;
            std::unordered_map<std::uint32_t, std::string_view> subsystemNames;
            const auto nameFor = [&](const sub0log::SubsystemId subsystem) -> std::string_view {
                const auto found = subsystemNames.find(subsystem.value_);
                if (found != subsystemNames.end()) {
                    return found->second;
                }
                return subsystemNames.emplace(subsystem.value_, merger.subsystemName(subsystem))
                                     .first->second;
            };
            const auto labelFor = [&](const sub0log::SubsystemId subsystem) -> const std::string& {
                const auto found = subsystemLabels.find(subsystem.value_);
                if (found != subsystemLabels.end()) {
                    return found->second;
                }
                const std::string_view name = nameFor(subsystem);
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
            if (merged.size() > options.maxRecords_) { throw BudgetError{"decoded record budget exceeded"}; }
            const auto totals = merger.totals();
            ++health.scans_;
            health.records_ = merged.size();
            health.totals_ = totals;
            health.inputErrorSeen_ = health.inputErrorSeen_ || scanError;
            if (options.waitMs_ && (scanError || totals.unreadableBytes_ != 0 || totals.undecodableRecords_ != 0)) {
                std::fprintf(stderr, "sub0log-cat: wait input is incomplete or unreadable\n");
                return 1;
            }
            bool matched = false;
            for (const sub0log::MergedRecord& entry : merged) {
                if (gCancelled != 0) { return 128 + gCancelled; }
                if (options.waitMs_ && *options.waitMs_ != 0 && std::chrono::steady_clock::now() >= deadline) {
                    std::fprintf(stderr, "sub0log-cat: wait deadline reached without a match\n");
                    return 3;
                }
                if (options.follow_) {
                    const std::array<std::uint64_t, 3> key{entry.processId_,
                        entry.record_.segmentGeneration_, entry.record_.recordOffset_};
                    if (seen.contains(key)) { continue; }
                    if (seen.size() >= options.maxRecords_) { throw BudgetError{"cursor record budget exceeded"}; }
                    seen.insert(key);
                }
                if (!wanted(options, entry.record_)) {
                    continue;
                }
                std::string line;
                if (options.format_ == OutputFormat::Jsonl) {
                    line = sub0log::formatJson(entry, nameFor(entry.record_.site_->subsystem_));
                } else {
                    line = wallTimeText(entry.alignedNs_) + "  pid=" + std::to_string(entry.processId_)
                        + " tid=" + std::to_string(entry.record_.ownerThread_) + "  "
                        + severityName(entry.record_.site_->severity_) + "  "
                        + labelFor(entry.record_.site_->subsystem_) + "  "
                        + sub0log::Decoder::format(entry.record_)
                        + (entry.record_.truncated_ ? " [truncated]" : "");
                }
                line += '\n';
                if (line.size() > options.maxOutputBytes_ - outputBytes) {
                    throw BudgetError{"output byte budget exceeded"};
                }
                outputBytes += line.size();
                if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size()) {
                    std::fprintf(stderr, "sub0log-cat: failed to write text/JSONL output\n");
                    return 1;
                }
                ++health.emitted_;
                matched = true;
                if (options.waitMs_) { break; }
            }
            if (std::fflush(stdout) != 0 || std::ferror(stdout) != 0) {
                std::fprintf(stderr, "sub0log-cat: failed to flush text/JSONL output\n");
                return 1;
            }
            if (!options.follow_ || (options.waitMs_ && matched)) {
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
            auto pause = std::chrono::milliseconds{200};
            if (options.waitMs_) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    std::fprintf(stderr, "sub0log-cat: wait deadline reached without a match\n");
                    return 3;
                }
                pause = std::min(pause, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            }
            std::this_thread::sleep_for(pause);
        }

    } catch (const BudgetError& error) {
        std::fprintf(stderr, "sub0log-cat: %s\n", error.what());
        return 4;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "sub0log-cat: %s\n", error.what());
        return 1;
    }
    if (!anySegmentOpened) {
        std::fprintf(stderr, "sub0log-cat: nothing readable in the given path(s)\n");
        return 1;
    }
    if (options.format_ == OutputFormat::Jsonl
        && (std::fflush(stdout) != 0 || std::ferror(stdout) != 0)) {
        std::fprintf(stderr, "sub0log-cat: failed to flush JSONL output\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options options{};
    if (!parseArguments(argc, argv, options)) { return 2; }
    if (options.helpOnly_) { return 0; }
    Health health;
    const int status = run(options, health);
    if (options.healthJson_) {
        // A single final machine object; ordinary diagnostics may precede it.
        // Counts describe the last completed scan, never repeated-scan sums.
        const auto count = [&](const std::uint64_t value) {
            return health.scans_ == 0 ? std::string{"null"} : "\"" + std::to_string(value) + "\"";
        };
        std::fprintf(stderr,
            "{\"schema\":1,\"kind\":\"health\",\"exit_code\":%d,\"scans\":\"%llu\","
            "\"records\":%s,\"unreadable_bytes\":%s,\"unwritten_bytes\":%s,"
            "\"undecodable_records\":%s,\"skipped_records\":%s,\"emitted_records\":\"%llu\","
            "\"input_error_seen\":%s,\"producer_dropped_records\":null}\n",
            status, static_cast<unsigned long long>(health.scans_),
            count(health.records_).c_str(), count(health.totals_.unreadableBytes_).c_str(),
            count(health.totals_.unwrittenBytes_).c_str(), count(health.totals_.undecodableRecords_).c_str(),
            count(health.totals_.skippedRecords_).c_str(), static_cast<unsigned long long>(health.emitted_),
            health.inputErrorSeen_ ? "true" : "false");
    }
    return status;
}
