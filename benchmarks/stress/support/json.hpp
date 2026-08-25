#pragma once

/** @file support/json.hpp
 *  @brief Just enough hand-rolled JSON rendering for --json's summary file
 *         (scenario, scale, counters, pass/fail). No third-party dependency
 *         for a few dozen fields; nanobench's own JSON writer solves a
 *         different shape (timing results), so this is not reuse of it.
 */

#include "scenario.hpp"

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace sub0log::stress {

/// Escapes a string for a JSON string literal: quote, backslash, control
/// characters. Names and messages here are our own text (scenario names,
/// failure messages) -- no untrusted input -- but escaping unconditionally
/// keeps the output valid regardless of what a future scenario prints.
inline void writeJsonString(std::ostream& out, std::string_view text)
{
    out << '"';
    for (const char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20u) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out << buf;
            } else {
                out << c;
            }
        }
    }
    out << '"';
}

/// Writes one ScenarioResult as a JSON object. Counters are written as JSON
/// strings (a scenario may report a mix of counts, seeds and durations
/// under one key set; keeping them all strings avoids guessing which are
/// safe to render as numbers).
inline void writeScenarioJson(std::ostream& out, const ScenarioResult& result)
{
    out << "{\"name\":";
    writeJsonString(out, result.name_);
    out << ",\"skipped\":" << (result.skipped_ ? "true" : "false");
    if (result.skipped_) {
        out << ",\"skipReason\":";
        writeJsonString(out, result.skipReason_);
    }
    out << ",\"passed\":" << (result.passed() ? "true" : "false");
    out << ",\"checksRun\":" << result.checksRun_;

    out << ",\"counters\":{";
    for (std::size_t i = 0; i < result.counters_.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        writeJsonString(out, result.counters_[i].first);
        out << ':';
        writeJsonString(out, result.counters_[i].second);
    }
    out << '}';

    out << ",\"failures\":[";
    for (std::size_t i = 0; i < result.failures_.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << "{\"invariant\":";
        writeJsonString(out, result.failures_[i].name_);
        out << ",\"message\":";
        writeJsonString(out, result.failures_[i].message_);
        out << '}';
    }
    out << ']';

    out << '}';
}

/// Writes the whole --json summary: one object with the overall verdict and
/// an array of per-scenario objects.
inline void writeSummaryJson(std::ostream& out, bool overallPassed,
                             const std::vector<ScenarioResult>& results)
{
    out << "{\"overallPassed\":" << (overallPassed ? "true" : "false");
    out << ",\"scenarios\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        writeScenarioJson(out, results[i]);
    }
    out << "]}\n";
}

} // namespace sub0log::stress
