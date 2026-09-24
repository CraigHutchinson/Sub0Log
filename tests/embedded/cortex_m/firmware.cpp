// Sub0Log on a real Cortex-M instruction set, run under QEMU (MPS2 boards:
// AN386 = Cortex-M4, AN505 = Cortex-M33). Cortex-M has no lock-free 64-bit
// atomics, so this is the split 32-bit protocol (detail/atomics.hpp)
// running for real, not simulated on a host.
//
// What it does, in order, and what fails the run (non-zero exit):
//   1. an in-memory Logger over a 16 KiB static buffer, SUB0LOG_PLATFORM_CUSTOM;
//   2. a record of each argument shape, including a continuation-chained
//      string, then enough records to exhaust the buffer (drops counted);
//   3. executed-instruction costs for emit, disabled emit and a raw claim,
//      from QEMU's -icount clock (see measure());
//   4. writes the buffer to a host file over semihosting, which CI decodes
//      with the desktop sub0log-cat -- the cross-architecture claim.
//
// Built and run by tests/embedded/cortex_m/run.sh (CI: embedded-cortex-m).

#include <sub0log/log.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

static_assert(sub0log::detail::cSplitAtomics,
              "a Cortex-M build must be on the split 32-bit protocol -- if this fires, the "
              "toolchain claims lock-free 64-bit atomics and the test is not testing it");

// ---------------------------------------------------------------------------
// Board: CMSDK APB timer 0 (MPS2), a free-running down-counter on the
// system clock. Under -icount shift=0 the virtual clock advances 1 ns per
// executed instruction, so ticks are proportional to instructions; the
// proportion is calibrated against a loop of known length, not assumed.

namespace {

struct CmsdkTimer {
    volatile std::uint32_t ctrl;
    volatile std::uint32_t value;
    volatile std::uint32_t reload;
};

#if defined(SUB0LOG_BOARD_AN505)
// SSE-200 TIMER0, secure alias: the image runs in the secure state.
CmsdkTimer* const gTimer = reinterpret_cast<CmsdkTimer*>(0x50000000u);
#else
CmsdkTimer* const gTimer = reinterpret_cast<CmsdkTimer*>(0x40000000u);
#endif

void timerStart()
{
    gTimer->ctrl = 0u;
    gTimer->reload = 0xFFFFFFFFu;
    gTimer->value = 0xFFFFFFFFu;
    gTimer->ctrl = 1u; // enable
}

std::uint32_t timerNow()
{
    return gTimer->value; // counts down
}

// Exactly 4 instructions per iteration (subs, bne, and two nops), so
// `iterations * 4` instructions: the calibration standard.
__attribute__((noinline)) void knownLoop(std::uint32_t iterations)
{
    asm volatile("1:\n"
                 "  nop\n"
                 "  nop\n"
                 "  subs %0, %0, #1\n"
                 "  bne 1b\n"
                 : "+r"(iterations)
                 :
                 : "cc");
}

double gInstructionsPerTick = 0.0;

void calibrate()
{
    constexpr std::uint32_t cIterations = 2'000'000u;
    const std::uint32_t start = timerNow();
    knownLoop(cIterations);
    const std::uint32_t ticks = start - timerNow();
    gInstructionsPerTick = static_cast<double>(cIterations) * 4.0 / static_cast<double>(ticks);
}

template <typename F>
double measure(const char* const name, const std::uint32_t repetitions, F&& body)
{
    const std::uint32_t start = timerNow();
    for (std::uint32_t i = 0; i < repetitions; ++i) {
        body(i);
    }
    const std::uint32_t ticks = start - timerNow();
    const double perOp = static_cast<double>(ticks) * gInstructionsPerTick
                       / static_cast<double>(repetitions);
    std::printf("cortex-m: %-28s %8.1f instructions/op  (%lu reps)\n", name, perOp,
                static_cast<unsigned long>(repetitions));
    return perOp;
}

// ---------------------------------------------------------------------------
// The platform hooks (docs/embedded.md). No RTC here, so the wall hook
// returns the monotonic reading, as the contract asks.

std::uint64_t gMonotonic = 0;

} // namespace

extern "C" std::uint64_t sub0log_platform_monotonic_ns(void) noexcept
{
    // Derived from the timer so timestamps advance with execution; the
    // absolute scale does not matter to the format, only monotonicity.
    const std::uint32_t now = ~timerNow();
    gMonotonic += 1u + (now & 0xFFu);
    return gMonotonic;
}
extern "C" std::uint64_t sub0log_platform_wall_ns(void) noexcept { return gMonotonic; }
extern "C" std::uint64_t sub0log_platform_process_id(void) noexcept { return 0x4D33u; }
extern "C" std::uint64_t sub0log_platform_thread_id(void) noexcept { return 1u; }

// ---------------------------------------------------------------------------
// thread_local on bare metal: one task, one TLS block. ARM's TLS ABI puts
// variables at thread pointer + 8 + offset; __aeabi_read_tp must preserve
// every register but r0, hence naked. An RTOS supplies its own (Zephyr:
// CONFIG_THREAD_LOCAL_STORAGE).

extern "C" {
alignas(8) std::byte gTlsBlock[8 + 256];
extern std::byte __tdata_start[];
extern std::byte __tdata_end[];
__attribute__((naked, used)) void* __aeabi_read_tp()
{
    asm volatile("movw r0, #:lower16:gTlsBlock\n"
                 "movt r0, #:upper16:gTlsBlock\n"
                 "bx lr\n");
}
}

// ---------------------------------------------------------------------------

namespace {

constexpr sub0log::SubsystemId cSensor{3};
constexpr sub0log::SubsystemId cRadio{4};

constexpr std::size_t cStorageBytes = 16u * 1024u;
alignas(8) std::byte gStorage[cStorageBytes];
alignas(8) std::byte gScratch[64u * 1024u]; // measurement-only segment

int gFailures = 0;
void check(const bool ok, const char* const what)
{
    if (!ok) {
        std::printf("cortex-m: FAILED: %s\n", what);
        ++gFailures;
    }
}

} // namespace

int main()
{
    // TLS image: copy .tdata (zero-length today, but that is the contract).
    std::memcpy(gTlsBlock + 8, __tdata_start,
                static_cast<std::size_t>(__tdata_end - __tdata_start));

    timerStart();
    calibrate();
    std::printf("cortex-m: calibration %.3f instructions/tick\n", gInstructionsPerTick);

    // ---- functional ------------------------------------------------------
    const std::pair<sub0log::SubsystemId, std::string_view> names[] = {
        {cSensor, "sensor"}, {cRadio, "radio"}};
    sub0log::Logger::Options options{};
    options.segment_.chunkBytes_ = 1024u;
    options.subsystemNames_ = names;

    auto logger = sub0log::Logger::createInMemory(gStorage, options);
    check(logger.valid(), "createInMemory");
    {
        sub0log::Logger::ScopedBind bind{logger};
        sub0log_info(cSensor, "boot {} {} {}", std::uint32_t{0xC0DE}, std::int64_t{-42}, 2.5);
        sub0log_warning(cRadio, "rssi {} on {}", std::int16_t{-71}, std::string_view{"ch37"});
        char longText[1300];
        for (std::size_t i = 0; i < sizeof(longText); ++i) {
            longText[i] = static_cast<char>('a' + i % 26u);
        }
        sub0log_debug(cRadio, "frame {}", std::string_view{longText, sizeof(longText)});
        for (std::uint32_t i = 0; i < 2000u; ++i) {
            sub0log_debug(cSensor, "tick {}", i);
        }
    }
    const auto stats = logger.stats();
    const auto usage = logger.usage();
    std::printf("cortex-m: chunks %lu/%lu dropped=%lu truncated=%lu\n",
                static_cast<unsigned long>(usage.chunksClaimed_),
                static_cast<unsigned long>(usage.chunkCount_),
                static_cast<unsigned long>(stats.droppedRecords_),
                static_cast<unsigned long>(stats.truncatedRecords_));
    check(stats.droppedRecords_ > 0u, "the full buffer counted drops");
    check(usage.chunksClaimed_ == usage.chunkCount_, "usage() reports the buffer spent");
    // The split claim never counts past the end: the u64 cursor word in
    // the header reads as exactly the chunk count, high half zero.
    std::uint64_t cursorWord = 0;
    std::memcpy(&cursorWord, gStorage + sub0log::wire::cNextChunkOffset, sizeof(cursorWord));
    check(cursorWord == usage.chunkCount_, "cursor word == chunk count (bounded claim)");

    // ---- performance -----------------------------------------------------
    sub0log::Logger::Options benchOptions{};
    benchOptions.segment_.chunkBytes_ = 4096u;
    auto benchLogger = sub0log::Logger::createInMemory(gScratch, benchOptions);
    {
        sub0log::Logger::ScopedBind bind{benchLogger};
        // Warm: definitions written, a chunk held.
        sub0log_info(cSensor, "fixed2 {} {}", std::uint32_t{0}, std::int32_t{0});
        measure("emit.fixed2", 200u, [](std::uint32_t i) {
            sub0log_info(cSensor, "fixed2 {} {}", i, static_cast<std::int32_t>(i));
        });
        measure("emit.string16", 200u, [](std::uint32_t) {
            sub0log_info(cSensor, "s16 {}", std::string_view{"0123456789abcdef"});
        });
        benchLogger.setThreshold(sub0log::Severity::Error);
        measure("emit.disabled", 5000u, [](std::uint32_t i) {
            sub0log_info(cSensor, "fixed2 {} {}", i, static_cast<std::int32_t>(i));
        });
        measure("loop overhead (empty body)", 5000u, [](std::uint32_t i) {
            asm volatile("" : : "r"(i));
        });
    }
    {
        // Raw claim cost against a resident segment: the bounded 32-bit
        // compare-exchange (LDREX/STREX) plus the chunk-header stamp.
        alignas(8) static std::byte claimArea[sub0log::wire::cCompactSegmentHeaderBytes
                                              + 400u * 128u];
        auto segment = sub0log::detail::Segment::createInMemory(claimArea, {.chunkBytes_ = 128u});
        measure("claim.claimChunk", 400u, [&](std::uint32_t) {
            auto writer = segment.claimChunk();
            asm volatile("" : : "r"(&writer) : "memory");
        });
        measure("claim.exhausted (refused)", 2000u, [&](std::uint32_t) {
            auto writer = segment.claimChunk();
            asm volatile("" : : "r"(&writer) : "memory");
        });
    }

    // ---- hand the buffer to the host --------------------------------------
    std::FILE* const out = std::fopen("cortex_m_segment.s0l", "wb");
    check(out != nullptr, "semihosting fopen");
    if (out != nullptr) {
        check(std::fwrite(gStorage, 1, cStorageBytes, out) == cStorageBytes, "semihosting fwrite");
        std::fclose(out);
    }

    std::printf("cortex-m: %s\n", gFailures == 0 ? "OK" : "FAILED");
    return gFailures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Startup: vector table and reset handler. Semihosting (rdimon) provides
// printf/fopen/exit on the host side of QEMU.

extern "C" {
extern std::byte __bss_start__[];
extern std::byte __bss_end__[];
extern std::byte __stack_top[];
void initialise_monitor_handles(void);
void __libc_init_array(void);
// crti/crtn are not linked (-nostartfiles); __libc_init_array calls these.
void _init(void) {}
void _fini(void) {}

[[noreturn]] void Reset_Handler()
{
    std::memset(__bss_start__, 0, static_cast<std::size_t>(__bss_end__ - __bss_start__));
    initialise_monitor_handles();
    __libc_init_array();
    std::exit(main());
}

[[noreturn]] void Fault_Handler()
{
    std::printf("cortex-m: FAULT\n");
    std::exit(3);
}

__attribute__((section(".vectors"), used)) void* const gVectors[16] = {
    __stack_top,
    reinterpret_cast<void*>(&Reset_Handler),
    reinterpret_cast<void*>(&Fault_Handler), // NMI
    reinterpret_cast<void*>(&Fault_Handler), // HardFault
    reinterpret_cast<void*>(&Fault_Handler), // MemManage
    reinterpret_cast<void*>(&Fault_Handler), // BusFault
    reinterpret_cast<void*>(&Fault_Handler), // UsageFault
    reinterpret_cast<void*>(&Fault_Handler), // SecureFault (v8-M)
};
}
