/** @file emit.bench.cpp
 *  @brief KPI group "emit": the producer hot path (R1) at several argument
 *         shapes, the disabled-site cost (R1.4), and the no-Logger-bound
 *         cost.
 */

#include "support/groups.hpp"
#include "support/temp_dir.hpp"

#include <sub0log/log.hpp>

#include <nanobench.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace sub0log::bench {

namespace {

constexpr sub0log::SubsystemId cEmitSubsystem{1};

} // namespace

void runEmitGroup(std::vector<ankerl::nanobench::Result>& allResults)
{
    // Two Bench objects, not one: nanobench clears a Bench's accumulated
    // Results whenever unit() is called with a *different* string than it
    // already holds (its documented way of keeping mixed-unit runs from
    // being compared against each other) -- so "call"-unit KPIs (no record
    // is ever written) and "record"-unit KPIs share a Bench each rather
    // than alternating unit() on one, which would silently drop everything
    // measured before the last unit() switch.
    ankerl::nanobench::Bench benchCalls;
    benchCalls.title("emit").unit("call").batch(1).performanceCounters(false);

    // emit.unbound: measured before any Logger exists in this function, so
    // Logger::active() is nullptr and the call costs exactly what R1.4
    // promises for "nothing bound": one relaxed load, nothing else. This
    // relies on every other group in the suite unbinding via RAII
    // (ScopedBind) before returning, which they all do.
    benchCalls.minEpochIterations(50'000)
        .run("emit.unbound", [] {
            sub0log_info(cEmitSubsystem, "unbound {} {}", std::uint64_t{1}, std::int32_t{-1});
            ankerl::nanobench::doNotOptimizeAway(sub0log::Logger::active());
        });

    // A big segment (512 MiB, default 64 KiB chunks -> ~8,192 chunks) so
    // nothing below comes close to exhaustion and no per-epoch Logger
    // recreation is needed. Worst case across the four KPIs that actually
    // write records: emit.fixed2/fixed4/string16 each write at most
    // minEpochIterations(20,000) * epochs(11, nanobench's default) =
    // 220,000 records of <= 60 bytes on the wire (~13 MB each), and
    // emit.string256 writes at most 5,000 * 11 = 55,000 records of ~300
    // bytes (~16 MB) -- roughly 55 MB total against 512 MiB of segment,
    // and each KPI's records are far smaller than a single chunk's ~1,300-
    // record capacity, so fragmentation between KPIs costs nothing that
    // matters either. Contrast with the "claim" group, which deliberately
    // runs close to its segment's limit.
    TempDir dir{"emit"};
    auto logger = sub0log::Logger::create(
        {.directory_ = dir.path().string(),
         .segment_ = {.segmentBytes_ = 512ull * 1024 * 1024}});
    if (!logger.valid()) {
        std::cerr << "sub0log-bench: emit group: Logger::create failed, skipping\n";
        allResults.insert(allResults.end(), benchCalls.results().begin(), benchCalls.results().end());
        return;
    }
    sub0log::Logger::ScopedBind bind{logger};

    const std::uint64_t u64Arg = 123456789u;
    const std::int32_t i32Arg = -42;
    const double f64Arg = 3.14159;
    const bool boolArg = true;
    const std::string str16(16, 's');
    const std::string str256(256, 'x');
    const std::string_view sv16{str16};
    const std::string_view sv256{str256};

    ankerl::nanobench::Bench benchRecords;
    benchRecords.title("emit").unit("record").batch(1).performanceCounters(false);

    benchRecords.minEpochIterations(20'000)
        .run("emit.fixed2", [&] {
            sub0log_info(cEmitSubsystem, "two ints {} {}", u64Arg, i32Arg);
            ankerl::nanobench::doNotOptimizeAway(logger);
        });

    benchRecords.minEpochIterations(20'000)
        .run("emit.fixed4", [&] {
            sub0log_info(cEmitSubsystem, "four mixed {} {} {} {}", u64Arg, i32Arg, f64Arg, boolArg);
            ankerl::nanobench::doNotOptimizeAway(logger);
        });

    benchRecords.minEpochIterations(20'000)
        .run("emit.string16", [&] {
            sub0log_info(cEmitSubsystem, "short string {}", sv16);
            ankerl::nanobench::doNotOptimizeAway(logger);
        });

    benchRecords.minEpochIterations(5'000)
        .run("emit.string256", [&] {
            sub0log_info(cEmitSubsystem, "long string {}", sv256);
            ankerl::nanobench::doNotOptimizeAway(logger);
        });

    // emit.disabled (R1.4): the threshold is raised above this call's
    // severity first, so enabled() returns false, the arguments are never
    // evaluated, and no chunk is claimed -- this KPI does not touch the
    // segment at all. (That "arguments not evaluated" property is asserted
    // by a unit test, not here; this benchmark only measures the cost of
    // the disabled path.) Back on benchCalls: same unit ("call") it already
    // holds, so nothing measured above is lost.
    logger.setThreshold(sub0log::Severity::Fatal);
    benchCalls.minEpochIterations(50'000)
        .run("emit.disabled", [&] {
            sub0log_info(cEmitSubsystem, "two ints disabled {} {}", u64Arg, i32Arg);
            ankerl::nanobench::doNotOptimizeAway(logger);
        });

    allResults.insert(allResults.end(), benchCalls.results().begin(), benchCalls.results().end());
    allResults.insert(allResults.end(), benchRecords.results().begin(), benchRecords.results().end());
}

} // namespace sub0log::bench
