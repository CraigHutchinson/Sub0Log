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

Run one with `ctest -L unit` (or `integration`, `system`).

### What runs where

Measured from the CI matrix, not estimated:

| platform | unit | integration | system | total |
|---|---:|---:|---:|---:|
| Linux (GCC, Clang, GCC+ASan/UBSan) | 17 | 22 | 13 | **52** |
| macOS (AppleClang) | 17 | 22 | 13 | **52** |
| Windows (MSVC) | 17 | 22 | 1 | **40** |

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
| R1.2 no allocation | encode refusal below + bench; sanitizer runs would surface hidden allocation as noise in `emit.*` |
| R1.3 no lock | design (one `fetch_add` claim) + bench `claim`, `throughput` scaling 1→4 threads |
| R1.4 disabled cost, args unevaluated | integration "with no Logger bound, the macro is a no-op and never evaluates its arguments"; bench `emit.disabled`, `emit.unbound` |
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
| R7.1 scoped instance, drain sync | every integration/system test uses `ScopedBind` over a temp dir |
| R7.2 no test-only branches | discipline + review; nothing in `include/` references a test macro |
| R8.1 three platforms | CI matrix, all six jobs green: Linux (GCC, Clang, GCC+ASan/UBSan) and macOS run 52/52; Windows/MSVC runs 40/40 (see "What runs where") |
| R8.2 C++23, no extensions | `CXX_EXTENSIONS OFF` everywhere; CI compilers enforce |
| R8.3 no producer deps | build: the library target links nothing; doctest/nanobench live in test/bench targets only |
| R9.1 drops counted | integration "Logger counts drops once a tiny segment is exhausted"; child stats tests |
| R9.2 unclassified more visible | unit severity ordering (Unclassified > Error); truncation flags asserted wherever a cap cuts |

The compile-time refusal (R1.2's sharpest edge) is a *negative concept
assertion*: `static_assert(!detail::Encodable<std::string>)` in
`unit/encode.test.cpp`. It fails the build, not a test run, which is the
point.

## Benchmarks (KPI collection)

`benchmarks/` (built with `-DSUB0LOG_BUILD_BENCHMARKS=ON`, Release only)
measures the claims the requirements make in cost terms: emit latency
(fixed/string args), disabled-site and unbound cost, chunk-claim cost,
multi-threaded throughput, decode and merge throughput, and `format()`
cost. `--json <path>` exports every result for trend collection; see
`benchmarks/README.md`.

## Known gaps, in one place

- R4 dlopen ABI round-trip (v2).
- Windows runs 40 of the 52 tests (see "What runs where"); the 12 it does
  not run are the POSIX-only `system` tests, which need the v2 Windows arms
  for process spawning and child capture.
- Continuation-chain and Blob record kinds: format reserved, no writer yet,
  so no tests beyond the reader skipping them.
- Power-loss durability: out of scope by design (`hard-kill.md`), not a gap.
