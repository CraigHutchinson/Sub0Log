# Sub0Log examples

A runnable ladder from "hello, typed record" to the multi-process, crash-surviving
case the library exists for -- and on past it, into the v2 surface: a plugin
across the C ABI, and what actually happens to an argument past the inline
cap. Each example is a single self-contained `.cpp` with its own `main()`
(one exception, noted where it appears); each prints readable output
explaining what it just did, and exits `0` on success.

## A note on the comments in this directory

Library code comments explain *why* to a maintainer who already knows the
shape of the thing. The comments in `examples/` are teaching material instead:
they explain *what and why* to a newcomer meeting Sub0Log for the first time,
which means they are more generous with prose than `STYLE_GUIDE.md` would
otherwise call for. This is a deliberate departure from the rest of the
codebase, not an oversight. It still stops short of narrating the obvious --
nothing here explains what `++i` does.

## Coverage matrix

Every consumer-visible capability the library has, and the example that
demonstrates it. This table is itself a deliverable, not a summary: the
ladder was written when the library was smaller than it is now, and the way
a gap like that stays found is by keeping the map of what covers what
somewhere a reader actually looks -- rather than in whichever maintainer's
head last audited it. Update this table in the same commit that adds or
retires an example.

| Capability | Requirement(s) | Example(s) | Notes |
|---|---|---|---|
| Typed records, no formatting at the call site | R1.1, R2.1 | 01 | |
| Disabled-call-site cost; one subsystem turned up without the rest | R1.4, R9.3 | 09 | |
| Consumer-owned subsystems, names carried in the segment | R2.2, R2.3 | 02 | `SubsystemDefinition` record |
| Testing your own logging code (scoped bind, field assertions, drop counter) | R2.3, R7.1, R7.2, R9.1 | 08 | |
| Durability across a signal handler and a hard kill | R3.1, R3.2 | 04 | POSIX-only -- precisely why is in the file, not "v2" |
| A reader tolerating a truncated/damaged segment tail | R3.3 | *(none)* | gap -- see below |
| Correlation across threads | R6.1, R6.2 | 03 | |
| Correlation across processes (env var, unmodified binary) | R5.4, R6.2 | 05 | POSIX-only -- raw `fork()`, precisely why is in the file |
| Correlation across the plugin boundary | R6.1 | 12 | |
| Multi-process merge, anchored timestamps | R5.1-R5.4 | 05 | |
| Child capture + interception, the mechanism | R5.5, R5.6 | 06 | now builds and runs on every platform |
| Child capture + interception, a real tool making real decisions | R5.5, R5.6 | 10 | `git`; harvest a hash, notice a failure, suppress boilerplate |
| Live tailing a segment being written | R3 (read side), R5.2 | 07 | |
| The counters, per-subsystem thresholds | R9.1, R9.3 | 09 | |
| Argument truncation past the continuation ceiling, made visible | R9.2 | 11 | `DecodedRecord::truncated_` |
| `std::string` as a call-site argument | *(adoption-friction.md 1.2)* | 11 | `StringViewLike` |
| Continuation chains: an argument between 512 and 4096 bytes arrives whole | *(record-model.md, "the medium case")* | 11 | |
| The plugin C ABI | R4.1-R4.3 | 12 | host + plugin, `-fvisibility=hidden` |
| `Merger::totals()` (read-side byte/record accounting) | R9.2 (read side) | *(none)* | gap -- see below |
| `sub0log-cat` | *(adoption-friction.md 2.1)* | *(none -- see below)* | root `README.md` + `tests/system/tool.test.cpp` |
| `find_package(Sub0Log)` consumption | *(adoption-friction.md 1.1)* | *(none -- deliberately)* | `tests/packaging` only |

Two real gaps this pass found and left open, in the order they are worth
closing:

- **A truncated/damaged segment tail (R3.3).** Every example reads a healthy
  segment; none hands the reader a deliberately truncated file and shows
  `SegmentReader::unreadableBytes()` reporting it, which is the whole
  point of R3.3 and R9.2. `tests/system/roundtrip.test.cpp` covers this at
  the test level; nothing in the ladder makes the shape of it visible to a
  reader who has not opened that file.
- **`Merger::totals()`.** 05 calls `merger.merged()` and never
  `merger.totals()` -- the read-side counterpart to the producer-side
  counters 09 already demonstrates (unreadable bytes, unwritten bytes,
  undecodable records) has no example of its own anywhere in the ladder.

Neither blocked this pass (the brief asked for the plugin ABI and
continuation chains "at minimum"), and both are cheaper to close than
either of those was -- a plausible next rung.

**On `sub0log-cat`, argued rather than assumed.** It is real, shipped,
documented in the root `README.md` with a worked command line, and driven
end-to-end against the actual built binary by
`tests/system/tool.test.cpp` -- so "no coverage" would be the wrong way to
read its `(none)` above. It does not get an `examples/NN_*.cpp` of its own
because the ladder's examples are consumer *code*: `#include
<sub0log/...>`, link `Sub0Log::Sub0Log`, and teach an API decision a
consumer makes in their own program. `sub0log-cat` is not that -- it is an
already-finished operator tool a consumer *runs*, not a pattern they write
against, and an example that only shells out to it and checks the exit
code would teach strictly less than the two lines already in the root
README and the real system test already do. If that changes -- if
`sub0log-cat` grows a scriptable mode meant to be driven *from* code rather
than a terminal -- this call is worth revisiting.

## The ladder

| # | File | What it teaches | Requirements |
|---|------|------------------|--------------|
| 01 | `01_hello.cpp` | Create a `Logger`, log a few typed records, read them back with `SegmentReader` + `Decoder`. Nothing formats at the call site. | R1, R2.1 |
| 02 | `02_subsystems.cpp` | The consumer, not the library, owns the subsystem vocabulary -- and can declare its names into the segment so a recovered file reads "Storage" rather than "subsystem 1". Filtering ("Storage records at Warning or above") is field comparisons, never a text search. | R2.2, R2.3 |
| 03 | `03_correlation.cpp` | `CorrelationScope` on several threads; joining an activity's records back together is an equality test on `correlationId_`. | R6.1, R6.2 |
| 04 | `04_crash_handler.cpp` (POSIX) | Logging *from inside a signal handler*, and a hard `SIGKILL` with no handler at all -- both survive with no graceful shutdown anywhere. The example the whole library is for. | R3.1, R3.2 |
| 05 | `05_multi_process.cpp` (POSIX) | A parent and a forked child (raw `fork()`, no `exec`) each write their own segment; `Merger` produces one time-ordered stream, correlation crossing the process boundary via the environment. | R5.1-R5.4 |
| 06 | `06_child_capture.cpp` | Capturing a real third-party subprocess's stdout/stderr as attributed records, with a `LineInterceptor` that suppresses noise and harvests a value live. The mechanism, with a synthetic script. | R5.5, R5.6 |
| 07 | `07_live_tail.cpp` | A miniature console tailer: a reader repeatedly re-opens the same segment a producer thread is still writing. The console view is a view. | R3 (read side), R5.2 |
| 08 | `08_testing_your_code.cpp` | How a consumer tests their own logging code with no test framework: `ScopedBind` over a temp directory, assertions on decoded fields, the drop counter. | R7.1, R7.2, R2.3, R9.1 |
| 09 | `09_operating.cpp` | What the mechanism says about itself: the two counters a metrics scrape should publish, the count of call sites that reached no instance at all, and turning one subsystem up without the traffic of every other one. | R9.1, R9.3, R1.4 |
| 10 | `10_child_capture_git.cpp` | The use case 06 only sets up: a real tool (`git`, against a throwaway repository) whose output is worth reacting to -- harvesting a commit hash, noticing a specific failure line, suppressing a boilerplate trailer. Skips cleanly (exit `0`) if `git` is not on the machine. | R5.5, R5.6, R9.1 |
| 11 | `11_continuation_chains.cpp` | What actually happens to a `std::string`/`std::string_view` argument over `cInlineBytesCap` (512) bytes: it arrives *whole*, reassembled from a bounded chain of further records, up to a 4096-byte ceiling -- past which it is truncated and the record says so. | R1.2, R2.1, R9.2 |
| 12 | `plugin_abi/host.cpp` + `plugin_abi/plugin.cpp` | R4's C ABI: a plugin built `-fvisibility=hidden`, linking nothing and including only `sub0log_abi.h`, logging into a host it never links against -- and staying decodable after the host unloads it. The one example in this ladder that is a host and a plugin rather than one file (a plugin, by definition, cannot be the same translation unit as its host). | R4.1-R4.3, R6.1 |

**On the POSIX-only entries, precisely.** 04 and 05 are POSIX-only because
of what they specifically do -- installing a POSIX signal handler, and
calling raw `fork()` to run this same binary twice sharing address space --
neither of which has a Windows equivalent, and neither of which is on this
project's roadmap (`docs/architecture.md`'s phasing has no entry for
either). That is narrower than "spawning a subprocess is POSIX-only", which
stopped being true in v2: `ChildProcess::spawn()`'s Windows arm
(`CreateProcessW`) shipped there, `windows-msvc` CI runs seven of the nine
process-spawning tests in `tests/system/child.test.cpp`, and 06 and 10 --
which only spawn through it -- build and run on every first-class platform
(R8.1). Each of 04 and 05 still has a Windows *build* arm: its `main()`
prints a one-line note explaining precisely why (not "this is a v2 item" --
that claim stopped being accurate once v2 shipped without it) and returns
`0`, so nothing disappears from `ctest -L example`'s point of view on
Windows.

10 additionally handles its one real external dependency gracefully: if
`git --version` cannot be spawned, it prints why and returns `0` rather
than failing the ladder. That is the right call for an *example* -- it
teaches a capability assuming the dependency is present, and must not fail
a build over an optional tool that has nothing to do with Sub0Log itself.
It would be the wrong call for a *test*, whose job is exactly to catch "git
vanished from the CI image" as the regression it is; nothing else in this
ladder makes that trade, because nothing else depends on a tool the library
does not ship.

## Building

From the repository root:

```sh
cmake -S . -B build -DSUB0LOG_BUILD_EXAMPLES=ON
cmake --build build
```

This adds one executable per example (`sub0log_example_01_hello` through
`sub0log_example_11_continuation_chains`, plus
`sub0log_example_12_plugin_abi_host` and its plugin,
`sub0log_example_12_plugin_abi_plugin`), each linking `Sub0Log::Sub0Log`
the same way any downstream consumer would -- except the plugin target
itself, which links nothing of the library at all (R4.1/R4.2); that is the
whole point of 12, and its own file comments explain why.

## Running

Run one directly:

```sh
./build/examples/sub0log_example_01_hello
```

Or, when `SUB0LOG_BUILD_TESTING` is also on (the default), run the whole
ladder as ctest smoke tests, labelled `example`:

```sh
ctest --test-dir build -L example --output-on-failure
```

Every example is self-contained: it creates its own scratch directory under
the system temp path, cleans it up before exiting, and finishes in a few
seconds with no arguments and no network access required (10 spawns `git`
locally against a throwaway repository it also creates and deletes; it
never touches the Sub0Log checkout it happens to be sitting in, and never
calls `clone`/`fetch`/`push`).
