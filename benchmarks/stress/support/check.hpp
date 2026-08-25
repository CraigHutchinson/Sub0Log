#pragma once

/** @file support/check.hpp
 *  @brief The one assertion primitive every scenario uses. The harness's
 *         product is the invariant check, not the number printed alongside
 *         it (see benchmarks/stress/README.md) -- so a failed check never
 *         aborts a scenario mid-run: it is recorded with a precise message
 *         and the scenario keeps going, so later invariants still get
 *         exercised and reported in the same pass.
 */

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sub0log::stress {

/// One failed check(), with a message naming the invariant, the expected
/// value and the actual one -- printed at the point of failure and again in
/// the end-of-run summary.
struct Failure {
    std::string name_;
    std::string message_;
};

/** Accumulates check() results for one scenario. Never throws, never
 *  aborts: ok() at the end says whether every invariant held.
 */
class Checker {
public:
    explicit Checker(std::string scenarioName) noexcept : scenario_{std::move(scenarioName)} {}

    /// Records one invariant check. `expected` and `actual` are rendered via
    /// operator<< -- every call site here passes a streamable type (an
    /// integer, a bool, or a string), so this stays free of a formatting
    /// dependency on the caller's type.
    template <typename Expected, typename Actual>
    bool check(bool condition, std::string_view name, const Expected& expected, const Actual& actual)
    {
        ++total_;
        if (condition) {
            return true;
        }
        std::ostringstream oss;
        oss << "invariant violated: " << name << " -- expected " << expected
            << ", actual " << actual;
        recordFailure(name, oss.str());
        return false;
    }

    /// For a check with no natural expected/actual pair (e.g. "did not
    /// crash"): still recorded with a precise message, never a bare abort.
    bool check(bool condition, std::string_view name)
    {
        return check(condition, name, std::string_view{"true"},
                     condition ? std::string_view{"true"} : std::string_view{"false"});
    }

    [[nodiscard]] bool ok() const noexcept { return failures_.empty(); }
    [[nodiscard]] const std::vector<Failure>& failures() const noexcept { return failures_; }
    [[nodiscard]] std::uint64_t totalChecks() const noexcept { return total_; }
    [[nodiscard]] const std::string& scenario() const noexcept { return scenario_; }

private:
    /// Prints the violation immediately (never a silent mid-scenario abort)
    /// and keeps it for the end-of-run summary.
    void recordFailure(std::string_view name, std::string message)
    {
        std::cerr << "FAIL " << scenario_ << ": " << message << "\n";
        failures_.push_back(Failure{std::string(name), std::move(message)});
    }

    std::string scenario_;
    std::vector<Failure> failures_{};
    std::uint64_t total_{0};
};

} // namespace sub0log::stress
