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
| `example` | `examples/` (8 programs) | consumer-shaped usage, run unattended so it cannot rot |

Run one with `ctest -L unit` (or `integration`, `system`, `example`).

Beside them sits **`Sub0LogStress`** (`benchmarks/stress/`, built with
`-DSUB0LOG_BUILD_BENCHMARKS=ON`), which is not a ctest label because it is
not a set of cases: it is one harness that puts the library under load and
asserts invariants, exiting nonzero when one breaks. See "Stress" below.

### What runs where

Measured from the CI matrix, not estimated:

| platform | unit | integration | system | total |
|---|---:|---:|---:|---:|
| Linux (GCC, Clang, GCC+ASan/UBSan) | 17 | 22 | 14 | **53** |
| macOS (AppleClang) | 17 | 22 | 14 | **53** |
| Windows (MSVC) | 17 | 22 | 2 | **41** |

The `example` label and the stress harness run on Linux in CI; the examples
that `fork` or spawn (04, 05, 06) are excluded from the Windows build by
`examples/CMakeLists.txt` rather than compiled and skipped, for the same
reason the system tests are.

`unit` and `integration` run identically everywhere -- the format, the
encoder, the reader and the recovery paths are all platform-agnostic by
construction, and that is the point of keeping them free of process and
filesystem machinery.

The 12 `system` tests Windows does not run are the ones that `fork` or spawn
a child: the hard-kill test, the two-process merge, the environment-root
correlation test, and the nine child-capture tests. Their Windows arms are
v2 (`docs/architecture.md` phasing). They are **compiled out** on Windows
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
| R1.4 disabled cost, args unevaluated | integration "with no Logger bound, the macro is a no-op and never evaluates its arguments"; bench `emit.disabled`, `emit.unbound`; integration `threshold.test.cpp` covers the per-subsystem levels the check now resolves, and the cold-path attribute on the unbound branch is what keeps `emit.disabled` at the number it was before R9.3 existed |
| R2.1 typed arrival | integration "decoder round-trips typed arguments…"; system "a logged record round-trips typed through the file" |
| R2.2 field filtering | decoded fields asserted per record (subsystem, severity, thread, correlation, time) across reader/merge/system tests |
| R2.3 assert on fields | every system test does exactly this; `format()` tests are the only text tests |
| R3.1 survives hard kill | system "committed records survive a hard kill of the producer" (fork, log, `_exit`) |
| R3.2 no flush path needed | same test: no shutdown code runs in the child |
| R3.3 truncated tail tolerated + counted | integration reader tests: uncommitted, torn, oversized-length, each with exact unreadable-byte counts |
| R3.4 generation vs stale data | integration "a chunk from a stale generation is skipped and its body counted"; unit wire tests pin the header layout |
| R4 plugin ABI | **gap** -- `sub0log_abi.h` ships; the dlopen round-trip test is v2 (named in architecture phasing) |
| R5.1 one segment per process | construction; system two-process test proves no shared write state exists to corrupt |
| R5.2 merge at read | integration merge tests (alignment, tie-breaks, totals) |
| R5.3 comparable clocks | integration "segments with different anchor pairs merge into aligned global order" -- epochs deliberately unrelated |
| R5.4 correlation into children | system "SUB0LOG_CORRELATION propagates into the child's environment"; "with no scope active, records carry the environment's root correlation"; unit env parsing |
| R5.5 non-cooperating child captured | system child tests: streams, exit codes, signals, caps, no-logger |
| R5.6 interception | system: suppress (counted), throwing matcher (logs anyway), harvest |
| R6 correlation is a field | unit scope tests; system round-trip asserts equality on the field |
| R7.1 scoped instance, drain sync | every integration/system test uses `ScopedBind` over a temp dir; system "a call site reached under a second Logger is announced into that segment too" is the regression for the defect that broke this for shared call sites |
| R7.2 no test-only branches | discipline + review; nothing in `include/` references a test macro |
| R8.1 three platforms | CI matrix, all eight jobs green: Linux (GCC, Clang, GCC+ASan/UBSan) and macOS run 53/53, Windows/MSVC 41/41, plus the `examples` and `stress-quick` gates (see "What runs where") |
| R8.2 C++23, no extensions | `CXX_EXTENSIONS OFF` everywhere; CI compilers enforce. One isolated exception, `SUB0LOG_COLD_PATH` (instance.hpp): no standard attribute says "keep this out of the caller", `[[unlikely]]` was tried and measured insufficient, and the comment carries both numbers |
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

- R4 dlopen ABI round-trip (v2).
- Windows runs 41 of the 53 tests (see "What runs where"); the 12 it does
  not run are the POSIX-only `system` tests, which need the v2 Windows arms
  for process spawning and child capture. Three of the eight examples are
  excluded there for the same reason.
- The stress harness and the examples run on Linux in CI only; neither has
  been exercised on macOS or Windows runners.
- Continuation-chain and Blob record kinds: format reserved, no writer yet,
  so no tests beyond the reader skipping them.
- Power-loss durability: out of scope by design (`hard-kill.md`), not a gap.
