# simdjson generation evaluation (#20), 2026-10-12

Status: evaluation complete; production adoption rejected. Root owns benchmarks/simdjson, benchmark CMake/README and this
record on perf/simdjson-evaluation, base29ef3aa64e768e9af00127f6490d4fd664dd2b11.
Serial CPU reservation; no workers. Production control is the merged direct-append
formatter. Candidate dependency: simdjson v4.6.4,
1bcf71bd85059ab6574ea1159de9298dcc1212c5. No default dependency or producer change.

Hypothesis: SIMD escaping and reusable contiguous builder storage improve cold
export, especially larger strings/batches, even after preserving legacy spelling
and copying to the owned return string. Falsifier: schema/byte incompatibility,
unacceptable platform/dependency cost, or matched process measurements show no
benefit in representative cases. Actual consumers: sub0log-cat and Crucible
RuntimeDiagnostics::writeJson (main7c6c461).

Correctness before timing: byte-for-byte output, integer extremes, nonfinite/signed
zero floats, invalid UTF8 hex fallback, controls, exceptions and no retained views.
Workloads: real decoded15/7/8-argument event shapes, short/long ASCII/UTF8,
escape-heavy and invalid text, record batches and actual consumer subprocesses.
Use existing nanobench; preserve five alternating independent process pairs,
all slower/failed results, source/binary hashes and compile/link commands. Shared
virtual host, uncontrolled thermal/frequency/load; advisory cache-hot results.

VTune remains unavailable; no sampled profile or hardware-counter claims.
O0 actual subprocess time includes startup/IO/decode; O1 format timing isolates
export; O2 fresh/reused builder plus owned copy; O3 avoid repeated scalar scans;
O4 inspect linked dispatch/escaping code and compiler remarks.

Pass1: compatibility adapter with fresh builder. Pass2: caller-owned reuse.
Pass3: bounded span appends for hex/control-free strings. Measurements retained below.
Production promotion requires default/variant/platform CI and actual consumer
receiving; benchmark-only success is not sufficient.

Before the qualifying campaign: require at least10% formatter improvement on all
three event shapes and no >5% median regression among the listed cases to recommend
further integration. Whole-process results must be reported separately, including
slower pairs; a microbenchmark win alone cannot justify a default dependency.
Pass4 buffers canonical control escapes in256-byte scratch after pass3's exploratory
escape-heavy regression. Qualifying arms retain passes1–4 and fresh/reused combinations.
Initial fixture setup failed because default chunk size exceeded64KiB; use4096-byte
chunks matching Crucible. This was fixed before any qualifying timing. The first
source build's ignored addSegment return warning was corrected to fail on decode setup.

## Final disposition (2026-10-13)

Retain the opt-in evaluation harness; do not adopt a production simdjson backend.
Pass5 compares a dependency-free standard-string reserve/bulk-append strategy.
The final matched campaign fails the predeclared no-regression gate: invalid-byte
hex output is slower for all final arms. Improvements on valid text and real
Crucible shapes do not erase that failure. The standard-library alternative also
misses the 10% admission-event threshold. No production performance claim follows
from this experiment.

The final campaign uses one immutable evaluator for all three arms and five
alternating independent control/candidate pairs per arm. Table entries below use
the median of paired percentage reductions in elapsed time, not a ratio of
unpaired medians; negative values mean slower. Every process first receives all
correctness cases. Dispatch selection remains inside the timed lambda. Batches
repeat one decoded event and are cache-hot, not independent record streams.

| Workload | stdlib | buffered fresh | buffered reuse |
| --- | ---: | ---: | ---: |
| admission15 | 8.9% | 30.6% | 31.9% |
| structural7 | 11.3% | 43.6% | 45.3% |
| mission8 | 16.2% | 45.7% | 46.0% |
| edges | 9.6% | 31.5% | 30.2% |
| short | 12.2% | 20.1% | 23.2% |
| ASCII4096 | 21.9% | 62.3% | 64.9% |
| UTF8 | 17.4% | 61.4% | 62.2% |
| escaped | 65.6% | 69.2% | 71.1% |
| invalid bytes | -23.0% | -23.0% | -19.6% |
| admission batch463 | 8.7% | 30.1% | 30.4% |

The invalid-byte regression appears in all five pairs: fresh simdjson reductions
range -28.6% to -19.4%; reuse -26.5% to -18.9%; stdlib -28.8% to -21.4%.
Earlier refinements did not show this consistently; both campaigns are retained,
not pooled. Shared-host variability and binary layout make attribution uncertain.

## Costs and limits

Linked text/data/BSS totals from GNU size: CLI control109,521 bytes, simdjson
fresh354,631, reuse354,515, stdlib112,265. Crucible control351,317,
simdjson586,415, stdlib355,658. These are linked-section sizes, not file size/RSS.
No measured allocation-count, peak-memory or build-time claim is made. Builder
reuse retains high-water capacity, escaping reserves worst-case space, and the
owned-output copy remains included. Failure mapping is explicit (MEMALLOC to
bad_alloc, other builder failures to runtime_error); allocation-failure injection
was not performed. Existing CLI receivers exercise budget/health/error behavior.

The Release GCC13.3 build uses -O3 -DNDEBUG -std=c++23, without native-ISA or
fast-math flags. The pinned dependency exports SIMDJSON_DEVELOPMENT_CHECKS even
with developer mode disabled; the captured commands retain this actual setting.
Disassembly confirms vector comparisons/masks in the linked fallback escaping
routine; runtime UTF8 kernels are also linked. This is evidence of emitted code,
not proof of a sampled hot path. No VTune or sampling profile was available.

The opt-in CI matrix receives Linux, macOS, Windows, Linux32 and ASan/UBSan.
Default ARM/embedded users remain dependency-free. Candidate ARM/embedded use is
not qualified by this local experiment; source portability alone is insufficient
for adoption. Reopen integration only with a concrete receiving consumer, a
resolved invalid-byte regression, acceptable size/memory cost and repeatable
end-to-end benefit on target hardware. Do not promote either candidate from
formatter-only wins.

## Evidence and review

[Evidence directory](simdjson-evidence/) retains raw samples and compile/link,
source and binary receipts. `refinements` holds the earlier six-arm campaign and
consumer measurements; scripts were extended afterward for the stdlib arm, so
its manifest identifies earlier source hashes, not the final source tree. Treat
those runs as exploratory refinement evidence. `final` is the decision campaign
against the final evaluator/generator and actual consumer binaries. No failed or
slower recorded timing run was removed. The early fixture configuration failure
and ignored-return warning were corrected before timing; a generator Path-name
error while adding stdlib was likewise fixed before its build/measurement.

Review scope: root performed a separate read of the C++ adapter/evaluator,
generator, CMake, CI and campaign/consumer scripts. This is self-review, not an
independent human approval. No installed header or producer path changes. Storage
lifetimes, bounded scratch writes, builder error propagation, copied output,
canonical escapes, float spelling and schema drift were checked. Actual CLI
receivers exercise JSON/follow behavior; Crucible receivers exercise normal and
exhausted diagnostics. Reproduction commands are in the benchmark README.

Final whole-process Crucible reductions (median of five paired reductions, each
process group contains20 independent launches): normal fresh -5.2% (range -7.0
to1.3), reuse5.2% (-1.1 to22.1), stdlib2.2% (-30.6 to12.6); exhausted fresh13.3%
(6.8 to20.5), reuse11.2% (0.0 to17.3), stdlib -2.4% (-7.3 to5.1). All1,200
launches succeeded. Startup/decode/IO dominate parts of these workloads; these
figures are not isolated serializer speedups. Each generated consumer binary
passed Crucible's independent normal/exhaust JSON receiver before measurement.

Final CLI control/candidates produced byte-identical18,056-byte JSON and identical
health output for the same eight-record image. All600 measured launches succeeded.
Whole-process paired reductions: fresh -1.9% (range -4.1 to8.3), reuse0.0%
(-6.8 to9.0), stdlib -4.5% (-10.5 to2.6). This campaign supplies no repeatable CLI
benefit. Fixture metadata is generated per campaign, so output length can differ
from the exploratory campaign without changing within-campaign equivalence.

Local final verification: all166 CTests passed in the Release opt-in build,
including the seven experimental tests. Python scripts compile successfully and
git diff whitespace checks pass. Hosted platform/sanitizer results are reported
on the PR at its exact tested commit; local receiving is not a substitute.

A separate diagnostic recompilation after timing used the same evaluator command
with only its object output redirected and -fopt-info-vec-optimized added. Compiler
remarks are retained alongside a bounded disassembly excerpt. No measured binary
was rebuilt or replaced during either final campaign.

Hosted first-head Windows receiving exposed an existing follow-fixture race:
run38332100748, job115083925401, simdjson::follow_reuse failed at the watched-file
rename with WinError32. The CLI snapshots use a short-lived std::ifstream; Windows
can deny deletion/rename while that handle is open. The receiving fixture now
retries only sharing violation32 for at most two seconds on rename/delete; all
other errors propagate, and identity/no-replay assertions remain unchanged. The
failure is retained here rather than hidden by rerunning the unchanged job. This
test-only correction does not alter the measured binaries or performance data.
