/** @file main.cpp
 *  @brief Entry point for Sub0LogBenchmarks: dispatches to the KPI groups
 *         in support/groups.hpp and, on request, renders every collected
 *         nanobench Result into one JSON file (the metric-collection
 *         artifact for CI trending -- see benchmarks/README.md).
 *
 *  Usage:
 *      Sub0LogBenchmarks                 run every KPI group
 *      Sub0LogBenchmarks --list          list the KPI group names and exit
 *      Sub0LogBenchmarks --json <path>   also write results to <path> as JSON
 *      Sub0LogBenchmarks emit claim ...  run only the named group(s)
 */

#include "support/groups.hpp"

#include <nanobench.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct GroupEntry {
    std::string_view name_;
    void (*run_)(std::vector<ankerl::nanobench::Result>&);
};

// One entry per KPI group; --list enumerates exactly this table, and a
// positional argument is checked against it by name.
constexpr std::array<GroupEntry, 6> cGroups{{
    {"emit", sub0log::bench::runEmitGroup},
    {"claim", sub0log::bench::runClaimGroup},
    {"throughput", sub0log::bench::runThroughputGroup},
    {"decode", sub0log::bench::runDecodeGroup},
    {"merge", sub0log::bench::runMergeGroup},
    {"format", sub0log::bench::runFormatGroup},
}};

void printUsage()
{
    std::cerr <<
        "Usage: Sub0LogBenchmarks [--json <path>] [group...]\n"
        "       Sub0LogBenchmarks --list\n"
        "\n"
        "  (no group given)  run every KPI group\n"
        "  --list            list the KPI group names and exit\n"
        "  --json <path>     also render every collected result to <path> as JSON\n"
        "  group...          run only the named group(s), in the order given\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> requestedGroups;
    std::string jsonPath;
    bool wantJson = false;
    bool wantList = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            wantList = true;
        } else if (arg == "--json") {
            if (i + 1 >= argc) {
                std::cerr << "sub0log-bench: --json requires a path argument\n";
                return 1;
            }
            wantJson = true;
            jsonPath = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "sub0log-bench: unknown option: " << arg << "\n";
            printUsage();
            return 1;
        } else {
            requestedGroups.emplace_back(arg);
        }
    }

    if (wantList) {
        for (const GroupEntry& group : cGroups) {
            std::cout << group.name_ << "\n";
        }
        return 0;
    }

    std::vector<const GroupEntry*> toRun;
    if (requestedGroups.empty()) {
        for (const GroupEntry& group : cGroups) {
            toRun.push_back(&group);
        }
    } else {
        for (const std::string& wanted : requestedGroups) {
            const auto it = std::find_if(cGroups.begin(), cGroups.end(),
                [&wanted](const GroupEntry& group) { return group.name_ == wanted; });
            if (it == cGroups.end()) {
                std::cerr << "sub0log-bench: unknown KPI group '" << wanted << "'\n";
                printUsage();
                return 1;
            }
            toRun.push_back(&*it);
        }
    }

    std::vector<ankerl::nanobench::Result> allResults;
    for (const GroupEntry* group : toRun) {
        std::cout << "\n=== " << group->name_ << " ===\n";
        group->run_(allResults);
    }

    if (wantJson) {
        std::ofstream out{jsonPath};
        if (!out.good()) {
            std::cerr << "sub0log-bench: could not open '" << jsonPath << "' for writing\n";
            return 1;
        }
        ankerl::nanobench::render(ankerl::nanobench::templates::json(), allResults, out);
        std::cout << "\nwrote " << allResults.size() << " result(s) to " << jsonPath << "\n";
    }

    return 0;
}
