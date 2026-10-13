#include <array>
#include <bit>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nanobench.h>
#include <sub0log/log.hpp>
#include "formatter.hpp"
#include "formatter-stdlib.hpp"

namespace {
/// Owns mutable site/text storage; deque keeps site addresses stable.
struct Case {
    std::string name_;
    sub0log::DecodedSite site_;
    sub0log::MergedRecord entry_;
    std::string text_;
};
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) { throw std::invalid_argument{"usage: Sub0LogJsonEvaluation arm results.json"}; }
        const std::string_view arm{argv[1]};
        if (arm != "control" && arm != "stdlib" && arm != "fresh" && arm != "reuse" && arm != "fast" && arm != "fast-reuse" && arm != "buffered" && arm != "buffered-reuse") {
            throw std::invalid_argument{"unknown arm"};
        }
        alignas(64) std::array<std::byte, 65536> image{};
        sub0log::Logger::Options options{};
        options.segment_.chunkBytes_ = 4096;
        auto logger = sub0log::Logger::createInMemory(image, options);
        if (!logger.valid()) { throw std::runtime_error{"fixture logger failed"}; }
        {
            const sub0log::Logger::ScopedBind bind{logger};
            constexpr sub0log::SubsystemId subsystem{1};
            // Actual Crucible wire shapes, emitted/decoded through the library.
            sub0log_warning(subsystem,
                "schema={} event={} run={} tick={} status={} action={} generation={} kind={} slot={} x={} y={} radius={} strength={} end_x={} end_y={}",
                std::uint64_t{1}, std::uint64_t{1}, std::uint64_t{1}, std::uint64_t{0}, std::uint64_t{1},
                std::uint64_t{2}, std::uint64_t{3}, std::uint64_t{1}, std::uint64_t{0},
                1.25f, 2.5f, 3.0f, 0.5f, 4.0f, 5.0f);
            sub0log_warning(subsystem, "schema={} event={} run={} tick={} status={} action={} generation={}",
                std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{1}, std::uint64_t{1},
                std::uint64_t{4}, std::uint64_t{2}, std::uint64_t{3});
            sub0log_info(subsystem, "schema={} event={} run={} tick={} status={} reclaimed={} target={} deadline={}",
                std::uint64_t{1}, std::uint64_t{3}, std::uint64_t{1}, std::uint64_t{4}, std::uint64_t{2},
                std::uint64_t{0}, std::uint64_t{1780}, std::uint64_t{4});
            sub0log_info(subsystem, "edges {} {} {} {} {} {} {} {}",
                std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::uint64_t>::max(),
                -0.0, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN(), '\0', std::string_view{"\xff\0", 2});
        }
        if (logger.stats().droppedRecords_ != 0) { throw std::runtime_error{"fixture dropped"}; }
        sub0log::Merger merger;
        if (merger.addSegment(image) != sub0log::SegmentError::Ok) {
            throw std::runtime_error{"fixture segment rejected"};
        }
        auto records = merger.merged();
        if (records.size() != 4) { throw std::runtime_error{"fixture decode count"}; }
        std::deque<Case> cases;
        const std::array names{"admission15", "structural7", "mission8", "edges"};
        for (std::size_t i = 0; i < records.size(); ++i) {
            cases.push_back({names[i], *records[i].record_.site_, records[i], {}});
            cases.back().entry_.record_.site_ = &cases.back().site_;
        }
        auto addText = [&](std::string name, std::string text) {
            cases.push_back({std::move(name), {}, {}, std::move(text)});
            auto& test = cases.back();
            test.site_.format_ = test.text_;
            test.site_.file_ = "fixture.cpp";
            test.entry_.record_.site_ = &test.site_;
        };
        addText("short", "hello");
        addText("ascii4096", std::string(4096, 'a'));
        std::string unicode, escaped, invalid;
        for (int i = 0; i < 512; ++i) {
            unicode += "\xc3\xa9\xf0\x9f\x98\x80";
            escaped += "\"\\\n\t\r\b\f";
            invalid += "\xff\xc0\x80";
        }
        addText("utf8", std::move(unicode));
        addText("escaped", std::move(escaped));
        addText("invalid", std::move(invalid));
        sub0log::simdjson_eval::Sink<false> scalar;
        sub0log::simdjson_eval::Sink<true> fast;
        sub0log::simdjson_eval::Sink<true> buffered;
        buffered.bulkControls_ = true;
        auto check = [&](const sub0log::MergedRecord& entry) {
            const auto expected = sub0log::formatJson(entry, "runtime");
            if (expected != sub0log::stdlib_eval::formatJson(entry, "runtime") ||
                expected != sub0log::simdjson_eval::formatJson(entry, "runtime", scalar) ||
                expected != sub0log::simdjson_eval::formatJson(entry, "runtime", fast) ||
                expected != sub0log::simdjson_eval::formatJson(entry, "runtime", buffered)) {
                throw std::runtime_error{"byte equivalence failed"};
            }
        };
        // Every arm is checked before any timing; reuse must not retain old bytes.
        for (int repeat = 0; repeat < 2; ++repeat) {
            for (const auto& test : cases) {
                check(test.entry_);
            }
        }
        // Boundary/tail bytes exercise both scalar and SIMD escaping, including
        // malformed UTF8. Floating bit patterns keep the control's exact spelling.
        sub0log::DecodedSite tailSite{};
        sub0log::MergedRecord tail{};
        tail.record_.site_ = &tailSite;
        for (std::size_t length = 0; length <= 65; ++length) {
            for (unsigned byte = 0; byte < 256; ++byte) {
                const std::string text(length, static_cast<char>(byte));
                tailSite.format_ = text;
                check(tail);
            }
        }
        auto edge = records.back();
        edge.processId_ = edge.alignedNs_ = edge.record_.ownerThread_ =
            edge.record_.monoNs_ = edge.record_.correlationId_ =
            edge.record_.segmentGeneration_ = edge.record_.recordOffset_ =
            std::numeric_limits<std::uint64_t>::max();
        std::mt19937_64 random{20};
        for (int i = 0; i < 4096; ++i) {
            edge.record_.args_[2] = std::bit_cast<double>(random());
            check(edge);
        }
        std::cout << "byte equivalence: " << cases.size() << " cases, two reuse cycles\n";
        auto format = [&](const sub0log::MergedRecord& record) {
            if (arm == "stdlib") return sub0log::stdlib_eval::formatJson(record, "runtime");
            if (arm == "control") return sub0log::formatJson(record, "runtime");
            if (arm == "reuse") return sub0log::simdjson_eval::formatJson(record, "runtime", scalar);
            if (arm == "fast-reuse") return sub0log::simdjson_eval::formatJson(record, "runtime", fast);
            if (arm == "buffered-reuse") return sub0log::simdjson_eval::formatJson(record, "runtime", buffered);
            if (arm == "fast" || arm == "buffered") {
                sub0log::simdjson_eval::Sink<true> temporary;
                temporary.bulkControls_ = arm == "buffered";
                return sub0log::simdjson_eval::formatJson(record, "runtime", temporary);
            }
            sub0log::simdjson_eval::Sink<false> temporary;
            return sub0log::simdjson_eval::formatJson(record, "runtime", temporary);
        };
        ankerl::nanobench::Bench bench;
        bench.title(std::string{arm}).unit("record").minEpochIterations(1000).performanceCounters(false);
        for (const auto& test : cases) {
            bench.run(test.name_, [&] { ankerl::nanobench::doNotOptimizeAway(format(test.entry_)); });
        }
        bench.batch(463).minEpochIterations(10).run("admission-batch463", [&] {
            for (int i = 0; i < 463; ++i) ankerl::nanobench::doNotOptimizeAway(format(cases.front().entry_));
        });
        std::ofstream output{argv[2]};
        ankerl::nanobench::render(ankerl::nanobench::templates::json(), bench.results(), output);
        if (!output) throw std::runtime_error{"result write failed"};
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
