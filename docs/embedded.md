# Embedded and mobile: caller-owned memory, what it guarantees, and what's left

Issue #2 asks for the producer to be separable from its storage, so that a
firmware or mobile target can hand Sub0Log a buffer instead of a mapped
file, and for the guarantees that change to be stated rather than implied.
This file is the design record for that work: what landed, what was
measured, and what is still open, with the reason each open item is open.

`memory.md` ("What a memory-restricted or embedded target would need")
listed the gap before any of this existed; the numbered items below refer
back to it.

## What landed

**`Logger::createInMemory(std::span<std::byte> storage, options)`.** The
same `Logger`, the same emit path, the same C ABI, thresholds and `Stats`,
writing the same wire format into memory the caller owns. `storage` is
zeroed at creation (caller memory is not a freshly truncated file, and a
previous run's committed records would otherwise read back as current),
then the ordinary segment header is written into it. After that, nothing on
the producer path knows the difference: `SegmentReader::open(storage)`
reads it in-process, and a dump of it decodes with `sub0log-cat`.

**`SUB0LOG_PLATFORM_CUSTOM`.** A third arm of `detail/platform.hpp` for
targets with no OS the header knows. It includes no OS header, file mapping
always fails (so only `createInMemory` is usable), `pthread_atfork` is not
registered, and the clock and identity come from four C functions the
consumer defines once:

```cpp
extern "C" std::uint64_t sub0log_platform_monotonic_ns(void) noexcept; // tick counter, in ns
extern "C" std::uint64_t sub0log_platform_wall_ns(void) noexcept;      // RTC, or 0
extern "C" std::uint64_t sub0log_platform_process_id(void) noexcept;   // image/node id
extern "C" std::uint64_t sub0log_platform_thread_id(void) noexcept;    // current task
```

### Where the seam went, and why not `BasicLogger<ChunkSource>`

`vnext-frontend-backend.md` sketched the seam as a template parameter on
`Logger`. Building it showed the seam belongs one level lower, at `Segment`,
for a reason that sketch did not have in front of it: **`Logger` is named,
concretely, by everything that finds the bound instance** --
`Logger::active()`, `ScopedBind`, `detail::emitRecord`, the C ABI's host
table. A `BasicLogger<MemorySegment>` is a different type, so either all of
those become templates (and every plugin boundary with them), or the active
binding becomes type-erased, which is a per-record indirection that R1 does
not allow.

`Segment` was already only "a span of bytes, a geometry, and whatever owns
the bytes". `claimChunk()` now reads `bytes_` rather than asking the
`FileMapping`, and there are two ways to fill `bytes_`: a file mapping, or
the caller's span. The decision is made once at creation and costs the
producer path nothing -- no template, no virtual, no branch. This is
`vnext-backends-and-memory.md`'s rung 1 with the seam in a better place; the
`ChunkSource` name is still right for the concept, it just did not need to
be a type parameter.

## What each backend guarantees

A backend chooses where bytes live and how durable they are, never what
they mean. Stated per backend, because "it's the same format" is not the
same claim as "it survives the same things":

| | file segment (`Logger::create`) | in-memory (`Logger::createInMemory`) |
|---|---|---|
| wire format, decoder, `sub0log-cat` | yes | yes -- identical bytes |
| no allocation, no lock on the emit path | yes | yes -- and none at creation either (measured, below) |
| survives the producer being hard-killed (R3.1) | **yes**: the pages belong to the kernel | **only if `storage` does**: ordinary process memory dies with the process. A retained-RAM region that survives a warm reset, or a shared mapping a supervisor also holds, keeps it; the library cannot tell which it was given and claims neither |
| survives power loss / kernel crash | no (never claimed: `hard-kill.md`) | no, unless the caller's storage is itself non-volatile |
| multi-process merge (R5) | yes | only after the caller dumps the buffer somewhere a reader can see it |
| full buffer | drops, counted in `Stats::droppedRecords_` (R9.1) | the same code path: drops, counted |
| fork safety | detached in the child | detached in the child (caller storage may be a shared mapping) |

The full-buffer row is not a new mechanism: exhausting a caller buffer is
exactly exhausting a file segment, so the existing counters are the
"explicit drop/truncation counters" the issue asks for, and they are tested
against a buffer sized to run dry (below).

## Evidence, against issue #2's acceptance criteria

| criterion | status | where |
|---|---|---|
| static-memory producer path: no heap allocation, no OS mapping call | **met**, measured | `tests/embedded/producer.cpp` replaces global `operator new` and counts across `createInMemory` + ~2000 records; the count must be zero. A deliberately injected `new` fails it. The segment path is empty -- there is no file |
| injected full-buffer behaviour tested, counters accurate | **met** | `memory_segment.test.cpp`: a buffer of exactly three chunks, 500 records; asserts `decoded + dropped == emitted`, and that the kept records are the oldest prefix, in order |
| no-exceptions, no-RTTI build of the embedded producer | **met** on GCC/Clang | `Sub0LogEmbeddedProducer` and `Sub0LogFreestandingProbe` are built `-fno-exceptions -fno-rtti`; MSVC builds with `/GR-` only (MSVC's STL does not support `_HAS_EXCEPTIONS=0`) |
| code size and RAM measured, not estimated | **met** for 32-bit ARM Cortex-A; **blocked** for Cortex-M (below) | numbers below; CI's `embedded-arm` job rebuilds and prints them |
| embedded-produced segment decodes with the desktop tooling | **met** | `embedded::decode` runs `sub0log-cat` on the buffer `embedded::producer` dumped |
| Android pause/resume/termination established by a test | **not done** | needs an NDK toolchain and an emulator in CI; see "Still open" |

Also covered: a previous run's records in the buffer do not reappear
(`memory_segment.test.cpp`, confirmed to fail with the zeroing removed);
misaligned, undersized and empty storage are refused with a reason; moving
an in-memory `Logger` leaves exactly one owner of the buffer.

## Measured cost

`tests/embedded/freestanding_probe.cpp`: an in-memory `Logger` over a
16 KiB static buffer, 1 KiB chunks, ten call sites of assorted argument
shapes, `SUB0LOG_PLATFORM_CUSTOM`. Built with `arm-none-eabi-g++` 13.2.1,
`-mcpu=cortex-a7 -marm -Os -fno-exceptions -fno-rtti -ffunction-sections
-fdata-sections -Wl,--gc-sections`, newlib `nosys.specs`. Sizes from
`arm-none-eabi-nm -S`, summed by where the symbol comes from.

**Flash**

| | 1 call site | 10 call sites |
|---|---|---|
| Sub0Log's own code (`sub0log::*` + `main`) | 3.9 KB | 8.8 KB |
| C++ runtime: `operator new`, EH personality, unwinder | 6.7 KB | 6.7 KB |
| `malloc`/`free` | 3.4 KB | 3.4 KB |
| `getenv`/`strtoull` | 1.1 KB | 1.1 KB |
| other libc/libgcc (`memcpy`, `memset`, 64-bit division, ...) | 9.1 KB | 9.1 KB |

So the library itself is about **4 KB plus roughly 0.5 KB per distinct
call site** (a site is a template instantiation over its argument types, so
sites sharing a shape share code). The ~11 KB of runtime above it is worth
looking at, because none of it is used at run time:

- **`operator new`, the unwinder and `malloc`** (~10 KB) are linked because
  `Logger::Options` and `Segment::path_` are `std::string` (the link map
  shows the probe's own object file referencing `operator new` and
  `operator delete`, and `new_op.o` pulling in `bad_alloc`, the EH runtime
  and `malloc` behind it). Short defaults
  sit in the small-string buffer, so nothing allocates -- the counter above
  proves that -- but `std::string`'s destructor still references
  `operator delete`, and `operator new` references `std::bad_alloc`. This
  is `memory.md` item 3, now with a number on it.
- **`getenv`/`strtoull`** (~1 KB) are `correlationFromEnvironment()`, R5.4's
  inherited correlation id -- meaningless without processes.

A firmware image that already has `malloc` and `memcpy` pays about 4 KB +
0.5 KB/site + ~7 KB; removing the strings would take the 7 KB away.

**RAM**

| | 32-bit ARM | x86-64 |
|---|---|---|
| `sizeof(Logger)` (wherever the caller puts it) | 192 B | 240 B |
| per logging thread: the writer cache (`thread_local`) | 32 B | 32 B |
| per call site: its `SiteDescriptor` (static, `constinit`) | 40 B | 56 B |
| the segment | the caller's buffer, exactly | the caller's buffer, exactly |
| of which, header area | 192 B (`wire::cCompactSegmentHeaderBytes`) | 192 B |

Plus a pointer for the active binding. Nothing else: no queue, no heap, no
worker thread.

## Custom endpoints and other firmware use cases: due diligence

Issue #2's real question is broader than "a RAM buffer": device firmware
wants records to end up in raw NVM, on a debug link, in a crash dump, or
somewhere bespoke. That was meant to be in scope from the start, so each
kind of endpoint was checked against what exists, by test or experiment
where one was possible, and the answer is written per endpoint rather than
as a general "covered".

**The organising fact:** the producer writes with plain stores and one
atomic, so its target must be *byte-addressable memory that behaves like
RAM*. Everything else is reached by *draining*: producers write RAM, and a
low-priority task moves finished segments to wherever they need to go,
entirely off the emit path. The in-memory backend covers the first kind
directly, and the drain pattern (below) covers the rest with no new
producer-side mechanism.

| endpoint | how | status |
|---|---|---|
| SRAM, TCM, PSRAM, any RAM | `createInMemory` over it | **covered**, tested |
| retained RAM across a warm reset (post-mortem, crash log) | `createInMemory` over the retained region; **read it before creating the next Logger**, because creation zeroes it | **covered**; the read-then-recreate order is tested ("post-mortem") |
| debugger / core dump (SWD, JTAG, a crash dump that includes RAM) | put the buffer at a known symbol; dump it (`dump binary memory` in GDB) and hand the file to `sub0log-cat` | **covered**: a dumped buffer is exactly what `embedded::decode` reads |
| memory-mapped, word-writable NVM (MRAM, FRAM, RRAM behind a mapped controller) | `createInMemory` over the mapped region | **works mechanically, with four caveats** (below); not tested on hardware |
| page/block NVM (internal or SPI/QSPI NOR flash, NAND, EEPROM, SD/eMMC, a filesystem such as littlefs) | drain: RAM segments, whole images programmed by a background task | **covered by the drain pattern**, tested with a stand-in sink; two findings (below) |
| streaming links (UART, SWO/ITM, RTT-style RAM rings, USB CDC, BLE, CAN) | drain, shipping images or claimed chunks | **the pattern fits**; lossy links need framing Sub0Log does not add (below) |
| a second core (AMP: application + network core) | one buffer per core, `process_id` hook = core id, merged like processes | **fits the multi-process model**; needs a shared timebase (below) |

### The drain pattern, and what the test proves

`memory_segment.test.cpp`, "A/B in-memory segments drained to a custom
sink lose nothing": two buffers; producers log into the bound one; a
control task watches `Logger::usage()` and, one chunk before the end, binds
a fresh `Logger` over the spare buffer and hands the full one to the sink
whole. 3000 records over dozens of rotations: zero dropped, every record
recovered in order by the ordinary reader.

`Logger::usage()` was added for this. `Stats` says what was lost; nothing
said how close loss was, and a rotator or drain cannot decide without it.
It is chunks claimed out of the total, from one relaxed load, clamped
because the raw cursor counts refused claims past the end. It is also what
`vnext-segment-rollover.md`'s layer 1 assumed `Stats` would provide.

Findings from building it:

- **Reuse needs a quiescent point.** After the swap, a producer that was
  mid-record in the old buffer is still writing there. Draining it
  meanwhile is safe (an uncommitted record is simply not there yet), but
  *re-creating a Logger over it* zeroes memory under that producer. The
  test is single-producer, so the point is trivially reached; with several
  producer tasks, the caller needs one (a scheduler lock, or every task
  having logged once since the swap). The library gives no "everyone has
  moved on" signal. That is open (item 7).
- **Ordering across rotated segments depends on the wall hook.** `Merger`
  aligns each segment through its own (monotonic, wall) anchor pair. With a
  wall hook returning a constant, as an RTC-less port naturally would,
  every segment's timeline restarts at the same instant: measured, 2999 of
  3000 records merged out of order. With the wall hook returning the
  monotonic reading, 0 out of order. The platform header and the probe now
  say so. On the host, anchors sampled milliseconds apart misordered 7
  records at one boundary through clock-sampling error, so a sink that
  knows its own order (a flash log does) should read in that order.
- **Erased flash reads as damage, not as empty.** NOR erases to `0xFF`; the
  format's "unwritten" is zero. Measured: an image whose unused space is
  `0xFF` still decodes every record, but the reader reports that space as
  `unreadableBytes()` rather than `unwrittenBytes()`, so health counters
  would flag damage that is not there. Programming whole images (as the
  test does) avoids it, since RAM supplies the zeros; a sink that programs
  only claimed chunks, to save flash, hits it. The fix is reader-side and
  additive (treat a chunk whose header is entirely erased as unwritten),
  and is open (item 8) rather than done here because it touches R3.4's
  "positive evidence" rule.
- **Never commit in place on page NVM.** Commit-last writes the payload,
  then the head word, into the same few bytes of storage. Flash with ECC
  program units (commonly 8 to 32 bytes) cannot program a unit twice, so
  records written directly to flash would corrupt themselves. Staging in
  RAM and programming whole images is the rule, not an optimisation.

### Memory-mapped NVM: the four caveats

A span over MRAM/FRAM/RRAM works, and the tests' logic applies unchanged,
but "the same code" is not "the same guarantees":

1. **Creation writes every cell.** `createInMemory` zeroes the whole
   region, once per Logger. On NVM that is wear and time, per boot.
2. **Stores cost NVM write time.** "No lock, no allocation" still holds;
   "costs what a RAM store costs" does not. Emit latency is bounded by the
   part's write timing, which the benchmark numbers do not describe.
3. **A store is not a persistence barrier.** Some controllers need a
   write-enable, or buffer writes until a flush. Without a barrier between
   payload and head word, a power cut can persist the head (which passes
   its commit tag) and lose the payload behind it. Power-loss safety on
   NVM therefore needs a platform hook at commit, which is a per-record
   cost and so belongs to a named NVM policy, never the default. Open
   (item 9).
4. **Survival is the part's, not the library's.** Consistent with the
   backend table above: the library does not know what the span is and
   claims nothing beyond what the memory itself gives.

Retained RAM has two caveats of its own: the region must be outside any
write-back data cache (or cleaned before reset), or a warm reset loses the
dirty lines; and ECC-protected SRAM can fault when read uninitialised after
a cold boot, so the "read the previous boot" step should be gated on the
reset reason.

### Other firmware concerns checked

- **Format strings and file paths go into the buffer.** A site's first use
  writes its definition record, format text and `__FILE__` included, into
  the segment. On a small buffer that is real space. Measured here: with
  128-byte chunks a definition carrying an absolute build path does not
  share a chunk with its message. `-ffile-prefix-map`/`-fmacro-prefix-map`
  shortens `__FILE__` at no cost and is the first thing a firmware build
  should set. Keeping strings out of the buffer entirely, resolved on the
  host from the ELF as dictionary-style firmware loggers do, would be a
  bigger saving and a real design question: the format is deliberately
  self-describing, and giving that up per target is a trade to make
  explicitly. Open (item 10).
- **Sleep and low power.** There is no background thread and nothing to
  wake: a quiet firmware costs nothing. Timestamps across sleep are only
  right if the monotonic hook keeps counting through it (an RTC-backed
  tick, not a clock that stops with the core).
- **Second core / DMA readers.** A buffer read by another core or a DMA
  engine needs cache maintenance the library does not do; one buffer per
  core avoids cross-core atomics altogether. Cores whose monotonic counters
  differ need the wall hook to return a shared timebase for merging.
- **Lossy streaming links.** The chunk header's generation gives a
  receiver a resynchronisation point, but nothing in the format detects a
  corrupted byte in a payload. A UART or radio sink should add its own
  CRC framing per shipped chunk.
- **Fault handlers.** Logging "last words" from a hard-fault handler is the
  ISR case (item 6), with one twist: the system is going down, so a
  handler may bind a dedicated fault `Logger` over its own small retained
  buffer. That gives it a fresh chunk and leaves the interrupted task's
  half-written chunk alone. It is a plausible recipe and is **not
  verified**, so it is recorded here, not recommended.

## Still open

**1. Cortex-M, and any core without lock-free 64-bit atomics.**
`chunk.hpp` `static_assert`s `std::atomic_ref<std::uint64_t>::is_always_lock_free`,
because the commit head word and the claim cursor are both u64, and a
library-substituted lock would make R1.3 quietly false. ARMv7-M and ARMv8-M
(every Cortex-M, including the recent parts with memory-mapped RRAM) have
32-bit exclusives only, so today the build refuses them -- deliberately.

The proposed way past it does **not** fork the wire format, which
`memory.md` item 5 assumed it would have to:

- *Commit.* The head word packs `payloadBytes | kind | flags` into its low
  32 bits and `sequence | commitTag` into its high 32 bits. On a
  little-endian core, storing the low half plainly and then the high half
  with a 32-bit release store writes exactly the bytes the 64-bit store
  does, and a reader that acquire-loads the high half first (the one
  holding the tag) and the low half second can never see a tag without its
  length. A desktop reader of a dump is not concurrent, so it is unchanged.
- *Claim.* The cursor at `wire::cNextChunkOffset` never exceeds the chunk
  count, which is already u32. A 32-bit compare-exchange on its low half,
  bounded so that it stops at the count instead of counting past it, keeps
  the high half zero -- identical bytes again -- and cannot wrap the way a
  bare 32-bit `fetch_add` could after four billion refused claims.
- *Cost.* A CAS loop instead of one `fetch_add` on the claim, which runs
  once per chunk, not per record. The commit stays one store.
- *Not covered.* ARMv6-M (Cortex-M0/M0+) has no exclusives at all; that
  needs an interrupt-masking critical section, which is a lock in all but
  name and should be a separately named, opt-in policy rather than a
  silent fallback. Big-endian cores need the halves swapped.

This touches the one piece of cross-thread synchronisation on the producer
path, so it is written down for review here rather than shipped in the
same change as the storage seam.

**2. `std::string` out of the configuration surface** (`memory.md` item 3,
`vnext-backends-and-memory.md` step 4). Now measured at ~7-10 KB of flash
on a target that did not otherwise need `operator new`. The shape is a
`createInMemory` overload that takes only a threshold, chunk size and
subsystem names -- nothing that can hold a `std::string` -- plus a
`Segment` that does not carry a path unless it is a file.

**3. A thread-local-free mode** (`memory.md` item 4). The writer cache is
`thread_local`. On the Cortex-A7 probe the compiler reads the thread pointer
straight from the CP15 thread-ID register (`mrc p15, 0, rN, c13, c0, 3`), so
it links with nothing extra -- but it only *works* if the RTOS sets that
register per task and lays out a `.tbss` block for each (Zephyr does, with
`CONFIG_THREAD_LOCAL_STORAGE`). Where that is not available, this needs a
single-writer mode with the cache as a plain `Logger` member.

**4. Android.** An NDK consumer test using app-private storage, and a test
that establishes -- rather than assumes -- what survives pause, resume and
termination. The file-segment path is POSIX and should need nothing new on
Android; what needs establishing is the process lifecycle (a backgrounded
app killed by the low-memory killer is a hard kill, which R3.1 covers for a
file segment and does not for an in-memory one). It needs an NDK toolchain
and an emulator job in CI, which this change does not add.

**5. A bounded-segment rotation/handoff recipe** for long-running sessions.
`vnext-segment-rollover.md` is the design; until it is built, the recipe is
"a new `Logger` per interval, merged at read time", which `README.md`'s
"Operating it" already describes.

**6. Keeping logging off ISR-equivalent paths.** Nothing here makes an emit
interrupt-safe: an interrupt that preempts a thread mid-record on the same
core would share that thread's writer cache. Until a per-context channel
exists (`vnext-backends-and-memory.md` step 2), the rule for firmware is the
issue's own: no logging from an ISR unless that specific path has been
verified safe.

**7. A quiescence signal for buffer reuse** with several producer tasks
(drain pattern, above). Options range from a per-thread epoch the rotator
can wait on to documenting a scheduler-lock recipe per RTOS.

**8. Erased-flash awareness in the reader:** report an all-erased chunk as
unwritten, not unreadable (measured above).

**9. An NVM persistence policy:** a platform barrier between payload and
head word for memory-mapped NVM that must survive power loss, opt-in and
named, because it costs every record.

**10. Strings out of the buffer:** `__FILE__` shortening is available
today; a dictionary mode (site text resolved from the firmware image on
the host) is a format-level decision, recorded rather than taken.

## Reproducing the numbers

```sh
arm-none-eabi-g++ -std=c++23 -mcpu=cortex-a7 -marm -Os -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -DSUB0LOG_PLATFORM_CUSTOM -Iinclude \
    --specs=nosys.specs -Wl,--gc-sections \
    tests/embedded/freestanding_probe.cpp -o probe.elf
arm-none-eabi-size probe.elf
arm-none-eabi-nm -C -S --size-sort probe.elf
```

The same file with `-mcpu=cortex-m4 -mthumb` stops at the `static_assert`
in `chunk.hpp` -- which is item 1, reproduced.
