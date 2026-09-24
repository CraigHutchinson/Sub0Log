// SUB0LOG_PLATFORM_CUSTOM with every hook constant -- the worst an RTOS
// port can hand the library (a tick that has not advanced, a fixed id), and
// exactly the A/B drain's shape: a Logger recreated over the same buffer,
// at the same address and stack depth, before the clock moves.
//
// Before the per-process counter in randomGeneration(), each recreation got
// the *same* generation, so this thread's cached writer (keyed on
// {Logger*, generation}) was reused into memory createInMemory() had just
// zeroed: the records vanished, with no drop counted. Each round below must
// decode exactly its own records.

#include <sub0log/log.hpp>
#include <sub0log/reader.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

extern "C" std::uint64_t sub0log_platform_monotonic_ns(void) noexcept { return 1000u; }
extern "C" std::uint64_t sub0log_platform_wall_ns(void) noexcept { return 1000u; }
extern "C" std::uint64_t sub0log_platform_process_id(void) noexcept { return 1u; }
extern "C" std::uint64_t sub0log_platform_thread_id(void) noexcept { return 1u; }

namespace {
alignas(8) std::byte gStorage[8u * 1024u];

// Same frame every round: noinline, called from the same loop.
[[gnu::noinline]] std::size_t oneRound(const std::uint32_t round, std::uint64_t& generation)
{
    sub0log::Logger::Options options{};
    options.segment_.chunkBytes_ = 1024u;
    auto logger = sub0log::Logger::createInMemory(gStorage, options);
    generation = logger.segmentGeneration();
    {
        sub0log::Logger::ScopedBind bind{logger};
        for (std::uint32_t i = 0; i < 5u; ++i) {
            sub0log_info(sub0log::SubsystemId{1}, "round {} record {}", round, i);
        }
    }
    auto reader = sub0log::SegmentReader::open(gStorage);
    sub0log::Decoder decoder;
    const auto records = decoder.decodeAll(reader);
    std::size_t mine = 0;
    for (const auto& record : records) {
        mine += std::get<std::uint64_t>(record.args_[0]) == round ? 1u : 0u;
    }
    return decoder.undecodableRecords() == 0u && records.size() == mine ? mine : 0u;
}
} // namespace

int main()
{
    int failures = 0;
    std::uint64_t previous = 0;
    for (std::uint32_t round = 0; round < 20u; ++round) {
        std::uint64_t generation = 0;
        const std::size_t decoded = oneRound(round, generation);
        if (decoded != 5u || generation == previous) {
            std::printf("FAILED: round %u decoded %zu of 5 (generation %s)\n",
                        static_cast<unsigned>(round), decoded,
                        generation == previous ? "repeated" : "fresh");
            ++failures;
        }
        previous = generation;
    }
    std::printf("constant clock: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
