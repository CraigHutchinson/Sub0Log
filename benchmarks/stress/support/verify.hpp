#pragma once

/** @file support/verify.hpp
 *  @brief Shared verification used by every scenario that logs a per-thread
 *         monotonically increasing counter and then checks the decoded
 *         argument values, not just the decoded count (R2.1/R2.3): a value
 *         that round-trips wrong, or one that was never emitted, is exactly
 *         as much a correctness failure as a record that went missing.
 */

#include "check.hpp"

#include <sub0log/reader.hpp>

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sub0log::stress {

/** For every decoded record, reads the std::uint64_t argument at `argIndex`
 *  and groups it by the record's owning (producer) thread. Per thread, the
 *  values must be unique (never invented, never duplicated) and each below
 *  `upperBoundExclusive` (the per-thread record count the scenario asked
 *  for). A record whose argument is not a uint64_t at all, or missing, is
 *  itself a violation -- this is what "verify values, not just counts"
 *  means in practice.
 *
 *  Returns the number of decoded records that carried a valid, in-range,
 *  non-duplicate counter -- callers fold this back into their own
 *  accounting where useful.
 */
[[nodiscard]] inline std::uint64_t
checkPerThreadCounters(Checker& checker, const std::vector<DecodedRecord>& records,
                       std::size_t argIndex, std::uint64_t upperBoundExclusive,
                       std::string_view invariantName)
{
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> seenByThread;
    std::uint64_t validCount = 0;
    bool everyValueSound = true;

    for (const DecodedRecord& rec : records) {
        if (argIndex >= rec.args_.size() ||
            !std::holds_alternative<std::uint64_t>(rec.args_[argIndex])) {
            everyValueSound = false;
            continue;
        }
        const std::uint64_t value = std::get<std::uint64_t>(rec.args_[argIndex]);
        if (value >= upperBoundExclusive) {
            everyValueSound = false; // out of the range this scenario could have emitted
            continue;
        }
        auto& seen = seenByThread[rec.ownerThread_];
        if (!seen.insert(value).second) {
            everyValueSound = false; // the same counter value decoded twice
            continue;
        }
        ++validCount;
    }

    checker.check(everyValueSound, invariantName, std::string_view{"true"},
                 everyValueSound ? std::string_view{"true"} : std::string_view{"false"});
    return validCount;
}

} // namespace sub0log::stress
