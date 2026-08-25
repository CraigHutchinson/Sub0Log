#pragma once

/** @file support/scenario.hpp
 *  @brief The scenario contract: one function per scenario, declared here
 *         and dispatched by name in main.cpp -- mirrors
 *         benchmarks/support/groups.hpp's one-function-per-group table.
 *
 *  A scenario never throws and never aborts on its own: it runs its
 *  workload, checks every invariant through a Checker (support/check.hpp),
 *  and returns a ScenarioResult describing what happened. main.cpp decides
 *  what that means for the process exit code.
 */

#include "check.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sub0log::stress {

/// Parsed CLI, threaded through to every scenario.
struct RunOptions {
    unsigned threads_{std::max(1u, std::thread::hardware_concurrency())};
    unsigned seconds_{3};
    bool quick_{false};

    /// Effective duration for a time-bounded scenario: honours --seconds,
    /// but --quick's contract (every scenario, under ~20s total) wins over
    /// a large explicit --seconds so `--quick --seconds 60` cannot blow the
    /// CI budget the flag exists to guarantee.
    [[nodiscard]] unsigned effectiveSeconds() const noexcept
    {
        return quick_ ? std::min(seconds_, 1u) : seconds_;
    }
};

/// One scenario's outcome: pass/fail plus the counters worth reporting,
/// in the order they should be printed. Values are pre-rendered to string
/// so a scenario can report a mix of counts, ids and flags without this
/// type needing to know their real types.
struct ScenarioResult {
    std::string name_;
    bool skipped_{false};        ///< e.g. child_flood on Windows.
    std::string skipReason_{};
    std::vector<Failure> failures_{};
    std::uint64_t checksRun_{0};
    std::vector<std::pair<std::string, std::string>> counters_{};

    [[nodiscard]] bool passed() const noexcept { return skipped_ || failures_.empty(); }
};

/// Builds a ScenarioResult from a finished Checker plus the counters worth
/// reporting. Every scenario's run*() function ends by calling this.
[[nodiscard]] inline ScenarioResult
finish(const Checker& checker, std::vector<std::pair<std::string, std::string>> counters)
{
    ScenarioResult result;
    result.name_ = checker.scenario();
    result.failures_ = checker.failures();
    result.checksRun_ = checker.totalChecks();
    result.counters_ = std::move(counters);
    return result;
}

/// A scenario reporting itself skipped (e.g. POSIX-only on Windows) rather
/// than run: skipped is not failed, and --list still names it.
[[nodiscard]] inline ScenarioResult skip(std::string name, std::string reason)
{
    ScenarioResult result;
    result.name_ = std::move(name);
    result.skipped_ = true;
    result.skipReason_ = std::move(reason);
    return result;
}

ScenarioResult runSaturate(const RunOptions& options);
ScenarioResult runOversubscribe(const RunOptions& options);
ScenarioResult runCapChurn(const RunOptions& options);
ScenarioResult runLiveTail(const RunOptions& options);
ScenarioResult runManySegments(const RunOptions& options);
ScenarioResult runChildFlood(const RunOptions& options);
ScenarioResult runSoak(const RunOptions& options);

} // namespace sub0log::stress
