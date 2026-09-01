# Test plan and requirements traceability

How the suite is organised, which test discharges which requirement, and the
gaps -- named, because an unnamed gap reads as coverage.

## Categories

Three ctest labels, three executables, one discipline each:

| label | executable | discipline |
|---|---|---|
| `unit` | `Sub0LogUnitTests` | pure logic against `wire.hpp`/`encode.hpp`/`context.hpp`; no I/O, no processes |
| `integration` | `Sub0LogIntegrationTests` | components against real files or hand-built images, one process |
| `system` | `Sub0LogSystemTests` | whole-library promises: hard kills, several processes, spawned children |
| `example` | `examples/` (12 programs) | consumer-shaped usage, run unattended so it cannot rot |

Run one with `ctest -L unit` (or `integration`, `system`, `example`).

Beside them sits **`Sub0LogStress`** (`benchmarks/stress/`, built with
`-DSUB0LOG_BUILD_BENCHMARKS=ON`), which is not a ctest label because it is
not a set of cases: it is one harness that puts the library under load and
asserts invariants, exiting nonzero when one breaks. See "Stress" below.

### What runs where

| platform | unit | integration | system | total |
|---|---:|---:|---:|---:|
| Linux (GCC, Clang, GCC+ASan/UBSan) | 25 | 36 | 29 | **90** |
| macOS (AppleClang) | 25 | 36 | 29 | **90** |
| Windows (MSVC) | 29 | 36 | 21 | **86** |

Linux and macOS are measured directly (`ctest --test-dir build`); Windows is
worked out the same way `SUB0LOG_EXAMPLES_POSIX_ONLY` below is -- by reading
which `TEST_CASE`s sit behind a `#if !defined(_WIN32)`/`#ifndef _WIN32` guard
-- rather than from an actual Windows run, so treat it as a confirmable
prediction and check it against CI before trusting it further.

The `example` label and the stress harness run on Linux in CI; the examples
that call `fork()` directly (04, 05) are excluded from the Windows build by
`examples/CMakeLists.txt` rather than compiled and skipped, for the same
reason the system tests are. 06 and 10 spawn a child too, but through
`ChildProcess::spawn()`, which has had a real Windows arm (`CreateProcessW`)
since v2, so both build and run there like everything else in the ladder.

`integration` runs identically everywhere -- the format, the encoder, the
reader and the recovery paths are all platform-agnostic by construction, and
that is the point of keeping them free of process and filesystem machinery.
`unit` is the same everywhere plus four Windows-only cases: `platform.test.cpp`
exercises `toWidePath()`, which only exists in `detail/platform.hpp`'s
`_WIN32` arm, so the whole file compiles to nothing anywhere else.

The 8 `system` tests Windows does not run are every one that calls `fork()`
directly -- the hard-kill test, the two-process merge, the environment-root
correlation test, and the forked-child-detach test (`roundtrip.test.cpp`),
plus the R9.3 detached-child-accounting test (`binding.test.cpp`) -- the
`sub0log-cat --follow` test (`tool.test.cpp`, fork/exec/SIGTERM to drive the
tool as a real subprocess), and 2 of the 10 child-capture tests
(`child.test.cpp`): the POSIX-signal case and the bad-executable-path case,
neither of which Windows can be asked (`docs/architecture.md`'s v2 phasing
note). The other 8 child-capture tests run on Windows since v2, same as
everywhere else -- child capture is no longer the all-or-nothing split this
table used to describe. Every exclusion above is **compiled out** on Windows
rather than skipped at runtime, so the count differing between platforms is
visible in the test output rather than hidden behind a green tick.

## Test doubles, and where mocking deliberately is not used

The seams are the ones production uses; there are no test-only branches
anywhere (R7.2). What stands in for what:

- **`tests/support/segment_image.hpp` -- the fake segment.** Reader and
  merge tests never run the producer; they build segment images byte by
  byte and inject damage precisely (torn head words, hostile lengths, stale
  generations). This is the mock that matters most, because it decouples the
  reader's contract from the code it would otherwise be tested against.
- **`Logger::ScopedBind` -- the instance double.** Tests bind a real
  `Logger` over a fresh temp directory (`tests/support/fixtures.hpp`) and
  read the file back synchronously. The file *is* the observable, so faking
  it would test the fake.
- **Interceptor fakes** in child tests: counting, suppressing, throwing and
  harvesting `LineInterceptor`s exercise R5.6 without a real consumer.
- **Environment injection** (`SUB0LOG_CORRELATION`, set/unset around a
  scope) stands in for a real parent process in unit/system correlation
  tests; a real forked parent covers the same path in the two-process test.
- **Not mocked: the clock and the filesystem.** Time ordering that matters
  is asserted through hand-set anchor pairs in built images (fully
  deterministic); mocking `monotonicNowNs()` would put a seam on the hot
  path R1 forbids. The filesystem is the durability claim itself -- a
  mocked mmap would vacate R3.

## Traceability

R-numbers from `REQUIREMENTS.md`. "bench" = the KPI suite under
`benchmarks/` (measured, not asserted).

| req | covered by |
|---|---|
| R1.1 no formatting | design: no format call exists on the emit path; bench `emit.*` bounds the cost |
| R1.2 no allocation | encode refusal below + bench; sanitizer runs would surface hidden allocation as noise in `emit.*`; the full ledger, including the one allocation that is not ours (dynamic TLS in a plugin), is in `docs/memory.md` |
| R1.3 no lock | `static_assert` that 64-bit atomics are lock-free, so a target that would silently substitute a lock table fails the build; design (one `fetch_add` claim) + bench `claim`, `throughput` scaling 1→4 threads; stress `oversubscribe` holds the accounting invariant at 4x cores, and ThreadSanitizer over every stress scenario reported no race |
| R1.4 disabled cost, args unevaluated | integration "with no Logger bound, the macro is a no-op and never evaluates its arguments"; bench `emit.disabled`, `emit.unbound`; integration `threshold.test.cpp` covers the per-subsystem levels the check now resolves (measured free -- removing the lookup changes nothing). R9.3's unbound counter is not free: about 0.15 ns per disabled site, 0.57 to 0.72, because the branchless compare has to become a branch. `SUB0LOG_COLD_PATH` recovers about 0.07 ns of that; instance.hpp carries the numbers and the correction to an earlier, larger claim |
| R2.1 continuation chains | integration `continuation.test.cpp`: a Bytes argument either side of the inline cap and up to the ceiling round-trips byte for byte; past the ceiling the cut is flagged and counted; long and short arguments mix without disturbing each other; and a chunk too small for a chain loses bytes but never records -- the last is the regression for a silent loss where abandoned reservations hid every record after them, since `SegmentReader::visit` stops at the first uncommitted head word |
| R2.1 typed arrival | integration "decoder round-trips typed arguments…"; system "a logged record round-trips typed through the file" |
| R2.2 field filtering | decoded fields asserted per record (subsystem, severity, thread, correlation, time) across reader/merge/system tests |
| R2.3 assert on fields | every system test does exactly this; `format()` tests are the only text tests |
| R3.1 survives hard kill | system "committed records survive a hard kill of the producer" (fork, log, `_exit`) |
| R3.2 no flush path needed | same test: no shutdown code runs in the child |
| R3.3 truncated tail tolerated + counted | integration reader tests: uncommitted, torn, oversized-length, each with exact unreadable-byte counts |
| R3.4 generation vs stale data | integration "a chunk from a stale generation is skipped and its body counted"; unit wire tests pin the header layout |
| R4 plugin ABI | `sub0log_abi.h` ships; the host-side implementation is `abi_host.hpp`. System `abi.test.cpp`: a plugin built `-fvisibility=hidden`, linking nothing of ours, `dlopen()`ed, handed the table, logs, `dlclose()`ed -- then decoded, proving R4.1 (no duplicated-instance failure) and R4.3 (decodable after unload) together. POSIX only; the Windows arm (`LoadLibraryW`/`GetProcAddress`/`FreeLibrary`) is written and compiled but CI-unverified, the same status as the rest of the v2 Windows work below |
| R5.1 one segment per process | construction; system two-process test proves no shared write state exists to corrupt |
| R5.2 merge at read | integration merge tests (alignment, tie-breaks, totals); system `tool.test.cpp` merges two processes' segments through the shipped `sub0log-cat` |
| R3/R5.1 concurrent readers | system `concurrent_readers.test.cpp`: two reader threads independently slurp-and-decode the same live, growing segment while a third thread produces into it -- no shared state between readers or with the producer to coordinate, since each reader is its own read-only file read plus its own `SegmentReader`/`Decoder` |
| tooling | system `tool.test.cpp` drives the real `sub0log-cat` binary: rendering, `--level`/`--subsystem` field filters, `--stats`, a bad path, declared subsystem names, and `--follow` picking up a record written after the tailer had already printed its first pass (POSIX: fork/exec/SIGTERM) |
| R5.3 comparable clocks | integration "segments with different anchor pairs merge into aligned global order" -- epochs deliberately unrelated |
| R5.4 correlation into children | system "SUB0LOG_CORRELATION propagates into the child's environment"; "with no scope active, records carry the environment's root correlation"; unit env parsing |
| R5.5 non-cooperating child captured | system child tests: streams, exit codes, signals, caps, no-logger |
| R5.6 interception | system: suppress (counted), throwing matcher (logs anyway), harvest |
| R6 correlation is a field | unit scope tests; system round-trip asserts equality on the field |
| R7.1 scoped instance, drain sync | every integration/system test uses `ScopedBind` over a temp dir; system "a call site reached under a second Logger is announced into that segment too" is the regression for the defect that broke this for shared call sites |
| R7.2 no test-only branches | discipline + review; nothing in `include/` references a test macro |
| R8.1 three platforms | CI matrix, all eight jobs green: Linux (GCC, Clang, GCC+ASan/UBSan) and macOS run 90/90, Windows/MSVC 86/86, plus the `examples` and `stress-quick` gates (see "What runs where") |
| R8.1 word size | `linux-gcc-m32` runs the whole suite under `-m32`. It exists for one claim -- a pointer is eight bytes on the wire on every target, so a 64-bit reader can decode what a 32-bit producer wrote -- which no local toolchain could check; unit `static_assert`s pin the invariant itself (`fixedWireSize<void*>() == 8`, `Encodable<void*>`) so a regression fails on 64-bit too |
| R8.2 C++23, no extensions | `CXX_EXTENSIONS OFF` everywhere; CI compilers enforce. One isolated exception, `SUB0LOG_COLD_PATH` (instance.hpp): no standard attribute says "keep this out of the caller", `[[unlikely]]` was tried and measured insufficient, and the comment carries both numbers |
| R8.1 non-ASCII paths | system `paths.test.cpp`: a real segment created under a `café-日本語` directory, written and decoded, on every platform -- the end-to-end half of the CreateFileW fix, whose unit test only covers the conversion in isolation. Its neighbour asserts an unusable directory produces an invalid Logger that counts drops rather than pretending (R9.2) |
| R8.1 installable | `packaging::find_package` installs to a throwaway prefix and builds an out-of-tree consumer against nothing else, on Ubuntu and Windows; the consumer sets no standard of its own, so C++23 and `/Zc:preprocessor` have to arrive as usage requirements or it fails |
| R8.3 no producer deps | build: the library target links nothing; doctest/nanobench live in test/bench targets only |
| R9.1 drops counted | integration "Logger counts drops once a tiny segment is exhausted"; child stats tests; stress `saturate` asserts emitted == decoded + dropped under exhaustion |
| R9.2 unclassified more visible | unit severity ordering (Unclassified > Error); truncation flags asserted wherever a cap cuts |
| R9.3 unreachable call site counted | system `binding.test.cpp`: unbound emits counted exactly, a bound instance never moves the counter (threshold-suppressed sites included), and a fork-detached child reports its own losses through its exit status |

The compile-time refusal (R1.2's sharpest edge) is a *negative concept
assertion* in `unit/encode.test.cpp`: `static_assert(!detail::Encodable<T>)`
for `std::vector<int>`, `std::filesystem::path` and
`std::chrono::milliseconds`, each of which needs a representation decision
only the call site can make. It fails the build, not a test run, which is
the point. `std::string` used to be on that list and is not any more: the
refusal's stated reason (a hidden allocation) was not true of a const
reference viewed as bytes, so it now has a *positive* assertion beside the
negative ones.

## Benchmarks (KPI collection)

`benchmarks/` (built with `-DSUB0LOG_BUILD_BENCHMARKS=ON`, Release only)
measures the claims the requirements make in cost terms: emit latency
(fixed/string args), disabled-site and unbound cost, chunk-claim cost,
multi-threaded throughput, decode and merge throughput, and `format()`
cost. `--json <path>` exports every result for trend collection; see
`benchmarks/README.md`.

## Stress (correctness under load)

`Sub0LogStress` is the counterpart to the KPI suite: where benchmarks
measure cost, this asserts that the promises still hold when the library is
pushed, and exits nonzero when one does not. Seven scenarios -- `saturate`,
`oversubscribe`, `cap_churn`, `live_tail`, `many_segments`, `child_flood`,
`soak` -- with `--quick` (2.3s) wired into CI as a real gate. The
load-bearing one is `saturate`'s **emitted == decoded + dropped**: a quick
run pushes 2401 records into a segment sized for 196 and the accounting
balances exactly.

Because it asserts rather than measures it can gate, which
`benchmarks-smoke` deliberately does not: shared runners make noisy
numbers, and a noisy gate is one people learn to ignore.

## Known gaps, in one place

- Windows runs 86 of the 90 tests (see "What runs where"); the 8 it does
  not run are the `system` tests that call `fork()` directly, which has no
  Windows equivalent and none planned. Process spawning and child capture
  themselves got their v2 Windows arms already -- only 2 of the 10
  child-capture tests are still POSIX-only, for reasons Windows genuinely
  cannot be asked (a signal, a shell-only failure mode), not because the
  capture path itself is unported. Two of the twelve examples (04, 05,
  both direct `fork()` callers) are excluded there for the same reason.
- The stress harness and the examples run on Linux in CI only; neither has
  been exercised on macOS or Windows runners.
- Blob record kind: format reserved, deliberately not built
  (`docs/record-model.md`, "A blob, for the cold bulk case" -- continuation
  chains already cover the case a blob would have); no tests beyond the
  reader skipping it. Continuation chains themselves are not a gap any more
  -- they shipped in v2 and are the R2.1 row above (`continuation.test.cpp`);
  this bullet used to cover both together and was not split when the writer
  landed.
- Power-loss durability: out of scope by design (`hard-kill.md`), not a gap.
