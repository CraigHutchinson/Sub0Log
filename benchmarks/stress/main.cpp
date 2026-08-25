/** @file main.cpp
 *  @brief Entry point for Sub0LogStress: dispatches to the scenarios in
 *         support/scenario.hpp (mirrors benchmarks/main.cpp's dispatch
 *         table). Numbers this prints are secondary; the assertions each
 *         scenario runs through support/check.hpp are the product -- exit
 *         code 0 means every invariant held, nonzero means at least one
 *         did not, and the message printed at the point of failure says
 *         which one, what was expected, and what actually happened.
 *
 *  Usage:
 *      Sub0LogStress                        run every scenario
 *      Sub0LogStress --list                 list scenario names, exit 0
 *      Sub0LogStress --scenario <name>      run one (repeatable)
 *      Sub0LogStress --threads N            producer threads (default: hardware_concurrency)
 *      Sub0LogStress --seconds S            duration for time-bounded scenarios (default 3)
 *      Sub0LogStress --quick                CI mode: smallest scale, still exercises every invariant
 *      Sub0LogStress --json <path>          write a machine-readable summary
 */

#include "support/json.hpp"
#include "support/scenario.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sub0log::stress::RunOptions;
using sub0log::stress::ScenarioResult;

struct ScenarioEntry {
    std::string_view name_;
    ScenarioResult (*run_)(const RunOptions&);
};

// One entry per scenario; --list enumerates exactly this table, and
// --scenario is checked against it by name -- the same shape as
// benchmarks/main.cpp's KPI group table.
constexpr std::array<ScenarioEntry, 7> cScenarios{{
    {"saturate", sub0log::stress::runSaturate},
    {"oversubscribe", sub0log::stress::runOversubscribe},
    {"cap_churn", sub0log::stress::runCapChurn},
    {"live_tail", sub0log::stress::runLiveTail},
    {"many_segments", sub0log::stress::runManySegments},
    {"child_flood", sub0log::stress::runChildFlood},
    {"soak", sub0log::stress::runSoak},
}};

void printUsage()
{
    std::cerr <<
        "Usage: Sub0LogStress [options] [--scenario <name>]...\n"
        "\n"
        "  (no options)         run every scenario\n"
        "  --list               list scenario names and exit 0\n"
        "  --scenario <name>    run only this scenario (repeatable)\n"
        "  --threads N          producer threads (default: hardware_concurrency)\n"
        "  --seconds S          duration for time-bounded scenarios (default 3)\n"
        "  --quick              CI mode: smallest scale, every invariant still exercised\n"
        "  --json <path>        write a machine-readable summary to <path>\n";
}

/// Parses a positive integer CLI argument. Returns false (leaving `out`
/// untouched) on anything that is not entirely digits -- an unknown/malformed
/// argument is the caller's job to report, not this function's to guess at.
[[nodiscard]] bool parseUnsigned(std::string_view text, unsigned& out)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

void printScenarioResult(const ScenarioResult& result)
{
    if (result.skipped_) {
        std::cout << "SKIP " << result.name_ << ": " << result.skipReason_ << "\n";
        return;
    }
    std::cout << (result.passed() ? "PASS " : "FAIL ") << result.name_ << " ("
              << result.checksRun_ << " checks";
    if (!result.passed()) {
        std::cout << ", " << result.failures_.size() << " failed";
    }
    std::cout << ")";
    for (const auto& [key, value] : result.counters_) {
        std::cout << " " << key << "=" << value;
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    RunOptions options{};
    std::vector<std::string> requestedScenarios;
    std::string jsonPath;
    bool wantJson = false;
    bool wantList = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            wantList = true;
        } else if (arg == "--quick") {
            options.quick_ = true;
        } else if (arg == "--scenario") {
            if (i + 1 >= argc) {
                std::cerr << "sub0log-stress: --scenario requires a name argument\n";
                return 1;
            }
            requestedScenarios.emplace_back(argv[++i]);
        } else if (arg == "--threads") {
            if (i + 1 >= argc || !parseUnsigned(argv[i + 1], options.threads_) || options.threads_ == 0u) {
                std::cerr << "sub0log-stress: --threads requires a positive integer argument\n";
                return 1;
            }
            ++i;
        } else if (arg == "--seconds") {
            if (i + 1 >= argc || !parseUnsigned(argv[i + 1], options.seconds_) || options.seconds_ == 0u) {
                std::cerr << "sub0log-stress: --seconds requires a positive integer argument\n";
                return 1;
            }
            ++i;
        } else if (arg == "--json") {
            if (i + 1 >= argc) {
                std::cerr << "sub0log-stress: --json requires a path argument\n";
                return 1;
            }
            wantJson = true;
            jsonPath = argv[++i];
        } else {
            std::cerr << "sub0log-stress: unknown argument: " << arg << "\n";
            printUsage();
            return 1;
        }
    }

    if (wantList) {
        for (const ScenarioEntry& entry : cScenarios) {
            std::cout << entry.name_ << "\n";
        }
        return 0;
    }

    std::vector<const ScenarioEntry*> toRun;
    if (requestedScenarios.empty()) {
        for (const ScenarioEntry& entry : cScenarios) {
            toRun.push_back(&entry);
        }
    } else {
        for (const std::string& wanted : requestedScenarios) {
            const auto it = std::find_if(cScenarios.begin(), cScenarios.end(),
                [&wanted](const ScenarioEntry& entry) { return entry.name_ == wanted; });
            if (it == cScenarios.end()) {
                std::cerr << "sub0log-stress: unknown scenario '" << wanted << "'\n";
                printUsage();
                return 1;
            }
            toRun.push_back(&*it);
        }
    }

    std::cout << "sub0log-stress: " << (options.quick_ ? "quick " : "") << "run, threads="
              << options.threads_ << " seconds=" << options.seconds_ << "\n";

    std::vector<ScenarioResult> results;
    results.reserve(toRun.size());
    bool overallPassed = true;
    for (const ScenarioEntry* entry : toRun) {
        ScenarioResult result = entry->run_(options);
        printScenarioResult(result);
        if (!result.passed()) {
            overallPassed = false;
        }
        results.push_back(std::move(result));
    }

    std::cout << (overallPassed ? "\nsub0log-stress: PASS -- every invariant held\n"
                                : "\nsub0log-stress: FAIL -- at least one invariant was violated (see above)\n");

    if (wantJson) {
        std::ofstream out{jsonPath};
        if (!out.good()) {
            std::cerr << "sub0log-stress: could not open '" << jsonPath << "' for writing\n";
            return 1;
        }
        sub0log::stress::writeSummaryJson(out, overallPassed, results);
        std::cout << "sub0log-stress: wrote summary for " << results.size() << " scenario(s) to " << jsonPath
                  << "\n";
    }

    return overallPassed ? 0 : 1;
}
