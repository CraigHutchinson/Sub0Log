// 11_continuation_chains.cpp -- what actually happens to an argument past
// the inline cap, because "there is a cap" invites the wrong guess about
// what it does.
//
// The natural assumption on meeting `wire::cInlineBytesCap` for the first
// time is that a cap silently cuts: pass more than 512 bytes and the rest
// is gone, the way a fixed-size C buffer would behave. That is not what
// this library does. A `std::string`/`std::string_view` (or `const char*`)
// argument over 512 bytes spills into a bounded chain of further records in
// the same thread's chunk (docs/record-model.md, "Continuation records, for
// the medium case") and comes back out of the decoder as one ordinary,
// whole argument -- a consumer never asks for the chain, never sees a
// Continuation record through the typed API, and never writes different
// code for a 40-byte argument and a 2,000-byte one. Only past a second,
// much wider ceiling -- cInlineBytesCap * (1 + cMaxContinuations), 4096
// bytes -- does truncation actually happen, and when it does the record
// says so in its own flags (DecodedRecord::truncated_) rather than handing
// back a shorter value with no sign anything was lost (R9.2).
//
// This example also closes a second, smaller gap: nothing else in this
// ladder happens to pass a std::string by value as a call-site argument.
// It compiles today (docs/adoption-friction.md 1.2 -- a StringViewLike
// argument copies no more than the view already would) and every call
// below uses one, on purpose.
//
// Requirements demonstrated: R1.2 (no unbounded producer-side cost -- the
// chain is a fixed number of further records, not an allocation), R2.1
// (the argument's original shape survives), R9.2 (loss past the ceiling is
// flagged, never silent).

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>
#include <sub0log/wire.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace {

constexpr sub0log::SubsystemId cApp{1};

// The two constants this whole example turns on, both ordinary public
// members of wire.hpp -- nothing below reaches into the library's
// implementation to know either of them.
constexpr std::size_t cInlineCap = static_cast<std::size_t>(sub0log::wire::cInlineBytesCap);
constexpr std::size_t cMaxContinuations = static_cast<std::size_t>(sub0log::wire::cMaxContinuations);
constexpr std::size_t cCeiling = cInlineCap * (1u + cMaxContinuations);

std::filesystem::path makeScratchDir()
{
    auto dir = std::filesystem::temp_directory_path() / "sub0log-example-11-continuation-chains";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path onlySegment(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".s0l") {
            return entry.path();
        }
    }
    return {};
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto* first = reinterpret_cast<const std::byte*>(raw.data());
    return {first, first + raw.size()};
}

// A recognisable, independently-verifiable fill: byte i is 'a' + (i % 26),
// so a reassembled or truncated copy can be checked byte-for-byte against
// this function rather than against a value carried around at runtime.
std::string patternOf(const std::size_t length)
{
    std::string out(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = static_cast<char>('a' + static_cast<char>(i % 26));
    }
    return out;
}

} // namespace

int main()
{
    std::printf("cInlineBytesCap=%zu  cMaxContinuations=%zu  ceiling=%zu bytes\n\n", cInlineCap,
                cMaxContinuations, cCeiling);

    const auto dir = makeScratchDir();
    auto logger = sub0log::Logger::create({.directory_ = dir.string(), .stem_ = "continuation"});
    if (!logger.valid()) {
        std::fprintf(stderr, "could not create a Logger\n");
        return 1;
    }

    // Deliberately not round numbers, so "512" and "4096" appearing in the
    // decoded output later are recognisably the library's own constants,
    // not an artifact of a suspiciously tidy input size.
    const std::string shortArg = patternOf(40);                  // well under the inline cap
    const std::string mediumArg = patternOf(cInlineCap + 1488);  // 2000: past the cap, under the ceiling
    const std::string oversizedArg = patternOf(cCeiling + 904);  // 5000: past the ceiling

    {
        sub0log::Logger::ScopedBind bind{logger};
        // std::string, not string_view{...}: every argument below is a
        // std::string passed by const&, exactly as adoption-friction.md 1.2
        // describes -- and every one is a std::string, not a wrapped view,
        // on purpose (see the file comment above).
        sub0log_info(cApp, "a short argument: {}", shortArg);
        sub0log_info(cApp, "a file path longer than the inline cap: {}", mediumArg);
        sub0log_info(cApp, "something that exceeds even the ceiling: {}", oversizedArg);
    }

    const auto image = slurp(onlySegment(dir));
    auto reader = sub0log::SegmentReader::open(image);
    if (!reader.valid()) {
        std::fprintf(stderr, "segment did not open\n");
        return 1;
    }

    // --- the raw wire: what one over-cap call site actually became ------
    //
    // Nothing above asked for this -- it is what SUB0LOG_EMIT built without
    // being told to. One Message record per call site, plus one
    // Continuation per further cInlineBytesCap-sized slice its overflowing
    // argument needed.
    std::size_t continuationRecords = 0;
    reader.visit([&](const sub0log::RecordView& v) {
        if (v.head_.kind_ == sub0log::wire::RecordKind::Continuation) {
            ++continuationRecords;
        }
    });
    // Ceiling-division: how many further cInlineBytesCap-sized slices an
    // overflow of this many bytes needs.
    const auto slicesFor = [](const std::size_t overflowBytes) {
        return (overflowBytes + cInlineCap - 1u) / cInlineCap;
    };
    const std::size_t mediumSlices = slicesFor(mediumArg.size() - cInlineCap);
    std::printf("raw segment: %zu Continuation record(s) on the wire\n"
                "  (%zu for the %zu-byte argument's overflow, "
                "%zu -- the cap -- for the one that exceeded the ceiling)\n\n",
                continuationRecords, mediumSlices, mediumArg.size(), cMaxContinuations);
    if (continuationRecords != mediumSlices + cMaxContinuations) {
        std::fprintf(stderr, "expected %zu Continuation records, found %zu\n", mediumSlices + cMaxContinuations,
                    continuationRecords);
        return 1;
    }

    // --- the typed layer: what a consumer actually gets back ------------
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    if (records.size() != 3) {
        std::fprintf(stderr, "expected 3 records, decoded %zu\n", records.size());
        return 1;
    }

    bool ok = true;
    for (const auto& record : records) {
        if (record.args_.size() != 1u || !std::holds_alternative<std::string_view>(record.args_[0])) {
            std::fprintf(stderr, "expected one string_view argument per record\n");
            return 1;
        }
        const auto arg = std::get<std::string_view>(record.args_[0]);
        std::printf("%-55s len=%-5zu truncated=%s\n", sub0log::Decoder::format(record).substr(0, 50).c_str(),
                    arg.size(), record.truncated_ ? "yes" : "no");
    }

    const auto shortArgOut = std::get<std::string_view>(records[0].args_[0]);
    const auto mediumArgOut = std::get<std::string_view>(records[1].args_[0]);
    const auto oversizedArgOut = std::get<std::string_view>(records[2].args_[0]);

    // 1. under the inline cap: unchanged, no chain, not truncated.
    ok = ok && (shortArgOut == shortArg) && !records[0].truncated_;

    // 2. past the inline cap, under the ceiling: the *whole* 2000 bytes
    //    comes back as one contiguous value -- the continuation chain
    //    above is exactly how, and the decoder's own class comment
    //    (reader.hpp, "One argument's view is the documented exception")
    //    is the only place a consumer needs to know that to understand why
    //    this string_view is not a view into the segment image the way
    //    every other argument on this page is.
    ok = ok && (mediumArgOut.size() == mediumArg.size()) && (mediumArgOut == mediumArg)
        && !records[1].truncated_;

    // 3. past the ceiling: cut at exactly cCeiling bytes, and *flagged* --
    //    the cut is visible in truncated_, not merely a shorter string that
    //    happens to look complete.
    ok = ok && (oversizedArgOut.size() == cCeiling) && (oversizedArgOut == oversizedArg.substr(0, cCeiling))
        && records[2].truncated_;

    std::filesystem::remove_all(dir);
    if (!ok) {
        std::fprintf(stderr, "one or more expectations about continuation chains did not hold\n");
        return 1;
    }
    std::printf("\nexample 11 OK\n");
    return 0;
}
