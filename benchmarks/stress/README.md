# Sub0LogStress

A correctness-under-pressure harness, not a benchmark. `benchmarks/` measures
*cost*; this puts the library under sustained concurrent load and **asserts
invariants** drawn straight from `REQUIREMENTS.md` -- principally R1.3 (no
lock on the producer path), R3.3/R3.4 (a reader tolerates a truncated tail
and rejects stale/recycled storage on positive evidence), and R9.1/R9.2 (a
drop, a suppression, or a truncation is counted and visible, never silent).
The numbers each scenario prints (throughput, read passes, line counts) are
secondary. The product is `Sub0LogStress`'s exit code: 0 only if every
invariant held across every scenario that ran.

## Running it

```sh
Sub0LogStress                        # every scenario, full scale
Sub0LogStress --list                 # scenario names, exit 0
Sub0LogStress --scenario saturate    # one scenario (repeatable)
Sub0LogStress --threads 8            # producer thread count (default: hardware_concurrency)
Sub0LogStress --seconds 5            # duration for the time-bounded scenarios
Sub0LogStress --quick                # CI mode: smallest scale that still exercises every invariant
Sub0LogStress --json out.json        # machine-readable summary alongside the console output
```

Exit code is 0 only if every scenario's invariants held; nonzero on the
first violation. Every failed check prints immediately, at the point it is
discovered, naming the invariant, the expected value and the actual one --
and the scenario keeps running afterwards, so a single fault never hides
the rest of what that run would have found.

## Scenarios

| name | what it hammers | invariant |
|---|---|---|
| `saturate` | N threads into a deliberately small segment | `emitted == decoded + dropped`; `undecodableRecords() == 0`; decoded argument values match what was emitted |
| `oversubscribe` | 4x `hardware_concurrency` threads into a large segment | same accounting invariant, under contention on the single `fetch_add` claim (R1.3); throughput reported, never asserted on |
| `cap_churn` | payloads straddling `wire::cInlineBytesCap` and the continuation-chain ceiling (`cInlineBytesCap * (1 + cMaxContinuations)`) | `cFlagTruncated` set iff over the chain ceiling, not the inline cap (a chained argument between the two arrives whole); `Logger::stats().truncatedRecords_` matches the over-ceiling count exactly; a truncated argument decodes to exactly the ceiling |
| `live_tail` | writers, plus a reader repeatedly opening/decoding the SAME live file | the reader never crashes; its decoded record count is monotonically non-decreasing across passes; `undecodableRecords()` stays 0 while writing continues |
| `many_segments` | M Loggers, distinct stems, one directory, merged | merged size == sum of independently-decoded per-segment counts; merged `alignedNs_` is non-decreasing; `Merger::totals().undecodableRecords_ == 0` |
| `child_flood` | POSIX only: `/bin/sh -c` printing thousands of lines across stdout/stderr | `capturedLines_` equals the exact line count; `suppressedLines_ + unloggedLines_ +` logged lines accounts for all of them; the on-disk `ChildExit` record shows exit code 0 |
| `soak` | producers + a live reader + periodic Logger recreation, for `--seconds` | the `saturate` accounting invariant at the end of every Logger generation's life; no growth in undecodable records across generations |

`child_flood` reports itself `SKIP`, not `FAIL`, on Windows (child capture's
Windows arm is v2 per `docs/architecture.md`).

## A subtlety every multi-Logger scenario used to work around

`SiteDescriptor::announcedGeneration_` (`include/sub0log/site.hpp`) is the
segment generation a site's SiteDefinition was last written into, keyed per
generation rather than a one-shot process-wide flag -- so a call site reused
across independently-built segments gets its own definition written into
each of them. That was not always true: the field used to be a plain
process-wide "announced" latch, so a call site reused across independently-
built segments in one process only wrote its SiteDefinition into the
*first* one, and every later segment silently produced undecodable Message
records. `benchmarks/support/mixed_records.hpp`, `many_segments`, and
`soak` (which recreates its Logger mid-run) each worked around the old
defect with a distinct call site per Logger/generation; the defect is fixed
now, and all three deliberately reuse **one** call site instead, which is
the honest test that the fix holds -- see each file's own header comment
for the history.

`saturate`, `oversubscribe` and `live_tail` hit a narrower version of the
same seam even with the fix: several producer threads can race the
`announcedGeneration_ != generation` check on their very first call. If the
loser of that race also happens to run out of segment room, it pays a
second, spurious drop for a definition nobody needed after the winner's
succeeded -- an artifact of the race this harness would introduce, not a
library fault. Each of those scenarios emits once, single-threaded, before
spawning producers, so the definition write is race-free and the
`emitted == decoded + dropped` accounting is exact rather than merely
probable.

## Determinism

Nothing here depends on wall-clock timing for pass/fail -- durations are
used only to bound how long a scenario runs, never compared against in an
invariant. `cap_churn`'s payload sizes are a fixed, explicit list rather
than drawn from an RNG, because the property under test is which *class*
(under/at/over the cap) a size falls into, not variety within a class.
Where a scenario's shape *is* randomised in the future, the discipline is:
seed explicitly, print the seed, so a failure is reproducible.
