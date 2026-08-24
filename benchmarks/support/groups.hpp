#pragma once

/** @file support/groups.hpp
 *  @brief One function per KPI group; main.cpp dispatches to these by name.
 *         Each appends its Bench objects' collected Results to `allResults`
 *         so --json can render every group's data into one file.
 */

#include <nanobench.h>

#include <vector>

namespace sub0log::bench {

void runEmitGroup(std::vector<ankerl::nanobench::Result>& allResults);
void runClaimGroup(std::vector<ankerl::nanobench::Result>& allResults);
void runThroughputGroup(std::vector<ankerl::nanobench::Result>& allResults);
void runDecodeGroup(std::vector<ankerl::nanobench::Result>& allResults);
void runMergeGroup(std::vector<ankerl::nanobench::Result>& allResults);
void runFormatGroup(std::vector<ankerl::nanobench::Result>& allResults);

} // namespace sub0log::bench
