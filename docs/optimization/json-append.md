# JSON export optimization, 2026-10-12

## Consumer and control

Owner: root/integrator, serial CPU reservation. Sub0Log control is main
2bd85d53ca72f185c303728de979aa3ccbdcc93f. Scope: json.hpp, existing format
benchmark, JSON regression tests and this evidence record. Actual callers are
sub0log-cat and Crucible RuntimeDiagnostics::writeJson (consumer main
70b557baf4c667e3d6a9891b0d2ecad2e6c0e23e). No producer-path change.

Hypothesis: direct integer appends remove temporary strings and reduce cold
JSON export CPU cost without changing a byte of output or input lifetimes.
Falsifier: matched Release formatter measurements show no improvement, or
consumer receiving/output equivalence fails. Public API, schema, exceptions,
owned return value and borrowed inputs must remain compatible.

## Diagnosis and campaign

O0: follow scans also pay file IO, decoding, sorting and cursor costs; a formatter
speedup is not a claim about those costs or end-to-end latency.
O1/O2: existing formatter repeatedly constructs numeric strings and concatenates
metadata; extend the existing nanobench format group to measure actual JSON.
VTune is absent (`command -v vtune` returned no executable): no sampled hotspot
or hardware-counter claim. O3: keep decimal conversion in the standard library; remove intermediate owned
strings and redundant quoting scans over known decimal digits.

Freeze matched benchmark binaries before five alternating process pairs; use
existing independent JSON receivers before timing. Shared hosted CPU, uncontrolled
frequency/temperature; results advisory. Preserve raw results and failures.

O4: inspected linked GCC Release `formatJson` disassembly: the old metadata
sequence calls `to_string` and string move/concatenation helpers; the new sequence
calls direct integer append. Symbol size falls from 8514 to 6123 bytes (not total
binary size). GCC vectorization remarks identify control-flow/string-growth
barriers in escaping and UTF8 loops; no SIMD improvement is claimed or required.
Call-site excerpts and JSON-header remarks are retained in
`json-append-codegen.json`; remarks were collected in a separate diagnostic
compile, never used as a timed binary. No no-alias assumption: all inputs remain
borrowed read-only and output is a fresh string.

## Results and review

Pass 1 selected: append decimal integers with `std::to_chars` into a stack
buffer, then append to the owned result. No capacity estimate, cache, alias
promise, SIMD/ISA change or fast-math flag. This is one successful mechanism;
there are no independent A/B mechanisms to combine and no negative result being
parked. Passes 2/3 are not claimed. Further reserve/escaping changes require a
separate hypothesis and workload; they are not prerequisites for this improvement.

Five pairs ran AB/BA/AB/BA/AB with one second idle between each process, no local
builds or other tests, unpinned virtual CPU and uncontrolled host load/power/thermal
state. Each process uses nanobench's warm repeated record (cache-hot); fixture IO
and decode are outside timing. Median JSON ns/record by pair:

| Pair | Control | Candidate |
| --- | ---: | ---: |
| 1 | 1073.38 | 833.46 |
| 2 | 1124.44 | 834.65 |
| 3 | 1070.27 | 855.95 |
| 4 | 1103.74 | 847.73 |
| 5 | 1074.31 | 847.32 |

Median of process medians: 1074.31 -> 847.32 ns (21.1% less time).
Every pair improves (20.0–25.8%). Unchanged text case spans 96.09–104.27 ns.
All raw epochs, stderr, unstable-run warnings, exploratory runs, binary/header
hashes, compile/link commands and environment are in `json-append-results.json`.
Release GCC13.3/libstdc++, `-O3 -DNDEBUG -std=c++23`, no native ISA/fast math.
No sampled profile or allocation-count claim; timings are advisory, not portable
latency guarantees. The CLI's repeated decoding/sorting and IO remain unchanged.

Correctness: Release full CTest 173/173, including JSON/follow receivers and the
new full-width metadata test. Old/new CLI stdout is byte-identical for the same
real eight-record edge fixture (18064 bytes). Existing parser covers signed
minimum/unsigned maximum, all integer widths, nonfinite floats, invalid UTF8,
controls, bytes, output errors and bounded follow semantics.

Separate root cpp-review pass (sub0/no overlay; not independent human approval):
base main2bd85d5; L0 actual CLI and Crucible callers consume the same API;
L1 helper remains feature-private, no new dependencies beyond standard concepts;
L2 result ownership, borrow duration, schema and exception classes retained;
L3 digits10+3 accommodates decimal maximum plus signed-minimum sign, no arithmetic
negation overflow, checked conversion, no dangling stack view. No blocking findings.
A failed initial Crucible prerequisite check lacked CMake/Ninja on PATH; corrected
existing tooling PATH and repeated successfully, without changing capability gates.

Disposition: candidate selected; merge remains gated on exact-head hosted CI.
Crucible pin/actual runtime and desktop receiving follow upstream merge. No claims
about game throughput, end-to-end shutdown latency, RSS or producer performance.
