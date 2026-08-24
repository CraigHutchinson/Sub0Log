# Sub0Log KPI benchmarks

Measures the claims REQUIREMENTS.md R1 makes about the producer path ("the
producer does no work it can defer") and R1.4 in particular (a disabled call
site costs a relaxed load and a comparison, nothing more), plus the
consumer-side costs (decode, merge, format) that R1.1 pushes off the
producer and onto whoever reads the stream. See `docs/architecture.md` for
the component map these KPIs are measured against.

Framework: [nanobench](https://github.com/martinus/nanobench) (fetched via
CPM, pinned to `v4.3.11`). One executable, `Sub0LogBenchmarks`, with a small
`main()` dispatching to six KPI groups.

## Building

```sh
cmake -S . -B build-bench -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSUB0LOG_BUILD_BENCHMARKS=ON \
    -DSUB0LOG_BUILD_TESTING=OFF
cmake --build build-bench
```

**Build type matters.** Benchmarks are meaningless below `Release` or
`RelWithDebInfo` -- an unoptimised build's timings say nothing about the
producer-path claims this suite exists to measure. For a single-config
generator (Ninja, Makefiles), `benchmarks/CMakeLists.txt` warns at configure
time if `CMAKE_BUILD_TYPE` is anything else. (A multi-config generator picks
the configuration at build time, e.g. `cmake --build . --config Release`, so
there is nothing to check at configure time for those.)

## Running

```sh
./build-bench/benchmarks/Sub0LogBenchmarks              # every KPI group
./build-bench/benchmarks/Sub0LogBenchmarks --list       # list the group names
./build-bench/benchmarks/Sub0LogBenchmarks emit claim   # only these groups
./build-bench/benchmarks/Sub0LogBenchmarks --json out.json   # also write JSON
```

`--json <path>` renders every collected `ankerl::nanobench::Result` --
across every group that ran, whether that was all six or a subset named on
the command line -- through nanobench's built-in JSON template into one
file. That file is the metric-collection artifact for CI trending: diff two
runs' `median(elapsed)` per named result to catch a regression, or graph a
series of them over time. An unrecognised group name, or an unrecognised
`--` option, exits non-zero without running anything.

The whole suite (all six groups, default settings) runs in a few seconds --
comfortably under the ~2 minute budget it was designed against.

## The KPI groups

### `emit` -- the producer hot path (R1)

A `Logger` bound over a 512 MiB segment (default 64 KiB chunks), then:

| KPI | measures |
|---|---|
| `emit.fixed2` | `sub0log_info` with two fixed-size args (u64, i32) |
| `emit.fixed4` | four mixed fixed args (u64, i32, f64, bool) |
| `emit.string16` | one 16-byte `string_view` arg |
| `emit.string256` | one 256-byte `string_view` arg |
| `emit.disabled` | the same two-arg call, but at a threshold that filters it out (R1.4) |
| `emit.unbound` | the same call with no `Logger` bound at all |

`emit.disabled` and `emit.unbound` do not write anything -- the point of
R1.4 is that a filtered-out call costs a relaxed load and a comparison and
stops there. This benchmark measures exactly that residual cost; the
*stronger* claim, that the call's arguments are never evaluated on the
disabled path (so `sub0log_trace(S, "{}", expensive())` never runs
`expensive()` unless `Trace` is enabled), is a correctness property asserted
by a unit test, not something a timing benchmark can demonstrate on its own.

### `claim` -- `Segment::claimChunk()` cost (R1.3)

The one cross-thread synchronisation on the producer path: a single
`fetch_add(relaxed)` on the segment's claim cursor. Measured directly
against `sub0log::detail::Segment`, with the iteration count fixed exactly
(`epochIterations`, not left to nanobench's usual auto-scaling) because a
claim permanently advances the segment and cannot be "redone" -- see the
comment in `claim.bench.cpp` for the exact numbers and why exhaustion is
approached (~81% of the segment's chunks) rather than recreating the
segment mid-run.

**Read this before trusting the number.** Every chunk claimed here is one
this process has never touched before, so the timed operation is the atomic
op *plus* the first-touch page fault for the `ChunkHeader` it then stamps.
`std::filesystem::temp_directory_path()` may resolve to real block storage
rather than `tmpfs` -- it does in this suite's own development sandbox
(`ext4` on a virtual disk) -- where first-touch can mean on-demand block
allocation and dominates the reported `ns/chunk` by two to three orders of
magnitude versus the bare atomic op, with correspondingly high run-to-run
variance (nanobench's own `:wavy_dash:` "Unstable" marker is expected here,
not a bug). That is a real cost paid on a process's first pass through a
freshly created segment; it is not what a claim against an
already-resident chunk costs in steady state (the common case once a
segment has been written once), which this KPI does not isolate.

### `throughput` -- sustained multi-threaded rate

`emit.fixed2`-shaped records from 1, 2 and 4 producer threads, each running
a fixed record count, against a fresh `Logger` per thread-count
configuration. Reported via `Bench::batch(recordCount)`, so nanobench prints
both `ns/record` and `record/s`. Expect `record/s` to scale up with thread
count (chunk claiming is the only shared state, and it is lock-free) rather
than to hold `ns/record` constant -- this KPI is about aggregate rate, not
per-thread latency. Each sample is one full spawn-emit-join pass across all
threads, so there are necessarily few, coarse-grained samples per thread
count; nanobench's "Unstable, increase minEpochIterations" suggestion does
not apply here (more, smaller samples is not a lever this benchmark has --
"a fixed record count in a tight loop" is the KPI as specified) and can be
read past.

### `decode` -- reader-side cost (R2)

A 100,000-record segment with a mix of argument shapes is built once via
the real producer path (unmeasured setup), slurped into memory once, then:

- `decode.recordsPerSecond`: `SegmentReader::open()` + `Decoder::decodeAll()`
  over the in-memory image, reported per record.
- `decode.bytesPerSecond`: the same operation, reported per byte of the
  image.

Both re-run the full open+decode from a fresh `SegmentReader`/`Decoder` each
iteration (a `Decoder` accretes a site table across calls, so reusing one
would measure decoding into an already-warm table from the second iteration
on, not the cold-start decode a reader actually performs).

### `merge` -- cross-process ordering (R5.2)

Four independent 25,000-record segment images (built the same way as
`decode`'s fixture, each with its own site definitions -- see the `Tag`
template parameter discussion in `support/mixed_records.hpp` for why that
matters), then `Merger::addSegment()` x4 followed by `merged()`, batched
over the 100,000 total records.

### `format` -- text rendering cost (the one consumer cost paid per line viewed)

`Decoder::format()` of a single decoded 4-argument record. Decoding
otherwise never touches text (R1.1); this is the one place it does, and
only on request.

## Design notes

- **Setup is never measured.** Every fixture -- segments, images, decoded
  records used as `format`'s input -- is built outside the timed lambda.
  `decode`, `merge` and `format` build their fixtures via the real producer
  path (a real `Logger`, real call sites) rather than hand-rolling the wire
  format, so the fixtures are exactly what the library would actually
  produce.
- **`doNotOptimizeAway` throughout.** Every measured lambda ends by handing
  its observable result (the `Logger`, a `ChunkWriter`, a decoded/merged
  vector, a rendered string) to `ankerl::nanobench::doNotOptimizeAway`, so
  the compiler cannot fold away work the benchmark exists to time.
- **One Bench per unit.** nanobench clears a `Bench` object's accumulated
  results whenever `unit()` is called with a string different from what it
  already holds. `emit` and `decode` each measure KPIs in more than one
  unit ("call" vs "record"; "record" vs "byte"), so each uses two `Bench`
  objects rather than alternating `unit()` on one -- alternating would
  silently drop everything measured before the last switch. (`main.cpp`
  collects every group's results the same way regardless: each
  `run*Group()` function appends whichever `Bench` objects it used.)
- **Temp directories** are created under `std::filesystem::temp_directory_path()`
  (mirroring `tests/support/fixtures.hpp`), tagged and pid-suffixed, and
  removed on scope exit (`support/temp_dir.hpp::TempDir`) or at the end of
  each fixture-building helper.
- **No platform guards.** Every group builds and runs identically on
  Windows/Linux/macOS; nothing here reaches for a POSIX-only API.
