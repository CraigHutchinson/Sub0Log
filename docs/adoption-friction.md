# Adoption friction: what a new consumer actually hits

The fork bug (`instance.hpp`, "Detach a forked child") was not found by
reading the requirements. It was found by writing the twenty lines a
consumer would write -- log, fork, keep logging -- and looking at what came
out: 41 records in, 21 decodable, every counter reporting zero. The
requirements had nothing to say about it because nobody had asked what
happens when an ordinary C++ program does an ordinary C++ thing.

So this file does that deliberately and to the whole surface. The method is
the same each time: take one thing a consumer plausibly does on their first
afternoon, do it, and record what actually happened -- not what the design
says should. Everything below has a measurement or a compile behind it.

The verdicts are three:

- **Bug** -- the library does the wrong thing, and the requirements already
  say so. Fix in v1.
- **Gap** -- nothing is wrong, but a consumer has to build something we
  could have shipped. Cheap to close and worth closing.
- **Deliberate** -- friction on purpose, kept because the alternative costs
  a property the library exists to provide. These still need to be *stated*
  where the consumer meets them, because undocumented deliberate friction is
  indistinguishable from an oversight.

**Read the findings in the present tense and the outcomes in the past.**
Each section below is kept as it was written -- the probe, the number, the
argument -- with what was done about it appended in bold. That is
deliberate: a finding with its own resolution attached is worth more than a
changelog entry, because the next person to wonder whether something is a
bug or a decision can see which one this was and why. Every item here is
resolved except 2.3, whose cause needs the v2 C ABI; "What this changed" at
the end is the summary.

## The pattern worth naming first

Three of the findings below are the same failure wearing different clothes,
and it is the failure R9.1 exists to forbid:

> A call site that cannot reach a segment is a no-op that reports success.

R9.1 says "a drop is never silent", and the emit path honours it exactly:
when a chunk cannot take a record, `droppedRecords_` moves. But every path
that fails *before* the chunk is reached bypasses the counter entirely --
there is no Logger to count on. A forked child, a call site in a plugin
compiled `-fvisibility=hidden`, and a process that simply forgot to bind all
produce identical observable behaviour: records vanish, `stats()` says zero,
`undecodableRecords()` says zero, the segment is intact.

That became a requirement rather than three fixes -- it is now **R9.3** in
`REQUIREMENTS.md`, and `sub0log::unboundEmits()` answers it for the whole
family at once: `enabled()` counts on the branch where it finds nothing
bound, so a forked child, a plugin with its own instance pointer and a
process logging before it binds all show up in the same number.
`Logger::detachedByFork()` remains beside it as the specific answer for the
specific case.

Two things about its shape are worth keeping, because both were arrived at
by measurement rather than taste. It is a **count**, not a flag, because a
handful means the startup window and a number that runs up at once means a
call site that will never reach a segment -- and it **saturates**, because
an unconditional atomic increment on that path measured 6.9 ns on one thread
and 104 ns per call on four, where the capped version is 0.77 and 0.91. And
it is **module-scoped**, which is not a limitation but the point: a plugin
that cannot see the host's instance cannot see the host's counter either, so
it reports its own, which is the granularity that makes the plugin case
legible from inside the plugin.

## 1. First hour: getting it into a build

### 1.1 `find_package(Sub0Log)` does not work -- **Gap, closed**

`CMakeLists.txt` declares `project(Sub0Log VERSION 0.1.0)` and an INTERFACE
target, and stops there: no `install(TARGETS)`, no `install(EXPORT)`, no
generated `Sub0LogConfig.cmake`. `grep -n "install|export(" CMakeLists.txt`
matches nothing.

So the only supported consumption is `add_subdirectory` or CPM. A consumer
whose build already has an install prefix -- a distro package, a vendored
SDK, a superbuild, Conan or vcpkg downstream -- cannot consume this at all
without writing our packaging for us. For a header-only library this is
perhaps thirty lines and it is the first thing a packager looks for.

Related: there is no `SUB0LOG_VERSION` macro anywhere in `include/`. A
consumer cannot conditionally compile against a version of us, and cannot
report which version produced a segment (the segment header carries the
*format* version, which is a different and deliberately slower-moving
number).

**Shipped.** `install(TARGETS/EXPORT)`, a generated `Sub0LogConfig.cmake`
with no `find_dependency` in it (R8.3 leaves nothing to find), and
`include/sub0log/version.hpp`. The proof is `packaging::find_package`, which
installs to a throwaway prefix and builds an out-of-tree consumer against
nothing else, on Ubuntu and Windows -- because "nobody tried it" was the
finding, and more inspection would only have reproduced it.

### 1.2 `std::string` does not compile -- **Deliberate, and wrong; closed**

Compiled today, each case its own TU (gcc 13, `-std=c++23`):

| argument | result |
|---|---|
| `Mode` (enum class), `std::size_t`, `float`, `void*`, `int` | compiles |
| `std::string_view{s}` | compiles |
| `std::string` | **rejected** |
| `std::chrono::milliseconds` | **rejected** |
| `std::filesystem::path` | **rejected** |
| `std::vector<int>` | **rejected** |

The refusals are the design working -- `record-model.md` is explicit that an
argument which would allocate or format at the call site does not compile,
and the diagnostic names the escape hatch. Three of those four are correct
and should stay: a `vector`, a `path` and a `duration` all need a decision
about representation that the consumer should make, not us.

`std::string` is the one to look at again, because **the stated reason does
not hold**. The message says "a `std::string` will not be silently copied --
that hidden allocation is what this refusal prevents". But a `std::string`
passed by const reference and inlined as bytes allocates nothing: it is
exactly the `string_view` path with the view taken for the caller, and the
argument's lifetime covers the full expression, temporaries included. What
the refusal actually costs is that the single most common argument type in
existing C++ code fails to compile, and the fix a consumer applies --
wrapping every string argument in `std::string_view{...}` -- produces
identical machine code to what we could have accepted.

This is the highest-volume friction in the library, and closing it does not
weaken the rule. The rule is "no allocation, no formatting at the call
site", not "no `std::string`". Accepting types with a non-throwing
conversion to `std::string_view` keeps the rule and deletes the paper cut;
the diagnostic then keeps its full force for `vector`, `path` and `duration`,
where it is right.

**Shipped.** A `StringViewLike` concept covering anything whose `const&`
converts to `std::string_view` without throwing, so `std::pmr::string` and
third-party string types come along. The three refusals beside it stayed,
and the diagnostic now explains them rather than asserting them. The
equivalence claim was checked rather than argued: the `std::string` and
`std::string_view` paths compile to the same instructions at the same
offsets.

### 1.3 Compile cost -- **Deliberate; now stated**

Mean of three, gcc 13, `-O2`, empty `main`:

| translation unit | wall |
|---|---|
| `int main(){}` | 16 ms |
| `<string>` + `<vector>` | 282 ms |
| `<sub0log/log.hpp>` | 439 ms |
| `+ <sub0log/reader.hpp>` | 659 ms |

Header-only means every TU that logs pays 423 ms over baseline, and about
220 ms over what a TU including `<string>` and `<vector>` already pays. On a
2,000-TU codebase that is real money. It is the price of the design and the
producer side is already the cheaper half -- but a consumer deserves the
number before they put `#include <sub0log/log.hpp>` in a widely-included
header, and `reader.hpp` should carry a note that it belongs in tools and
tests rather than in producer TUs.

**Stated**, in the README under "Operating it" -- where somebody deciding
whether to put the include in a widely included header will meet the number
before they do it.

## 2. First day: getting records out

### 2.1 Nothing ships that prints a segment -- **Gap, closed**

There is no `tools/` directory. `Decoder::format()` renders a record to a
string and works well; every consumer of this library will nonetheless write
the same forty-line loop -- open file, `SegmentReader::open`, `decodeAll`,
print -- before they can see anything at all. `REQUIREMENTS.md` lists
"whether the decoder ships as a library, a CLI, or both" as an open
question. The evidence from the examples is that the answer is both: the
first thing every example had to do was grow its own printer.

A `sub0log-cat` that takes files or a directory, merges them, and prints,
with `--follow` reusing the live-tail path example 07 already exercises, is
a day's work and changes the library from "a format plus a decoder" into
something a person can use on a Tuesday.

**Shipped**, near enough as described: `tools/sub0log-cat`, built and
installed by default, merging a directory of segments onto one timeline and
filtering on fields rather than by grep. A system test drives the real
binary with its real command line, since being the thing a person runs is
the whole of what it is for.

### 2.2 A segment cannot name its own subsystems -- **Gap, closed**

`SubsystemId` is documented as "opaque, consumer-defined. The consumer keeps
the name table; a decoder labels what it reads with that table, and the
library has no opinion."

That is defensible until you hold the format's own principle next to it.
`record-model.md` calls the format *strictly self-describing*: a segment
carries the definitions of its own call sites precisely so it can be decoded
alone, years later, with no access to the producing binary. Subsystems are
the one axis where that promise stops -- a segment recovered from a customer
machine decodes to `subsystem 77`, and the table that says what 77 means
lives in a header nobody kept.

The machinery to fix it already exists: a `SubsystemDefinition` record,
announced on first use exactly as a site definition is, keyed on segment
generation exactly as a site definition is. It costs one record per
subsystem per segment.

**Shipped**, with one change of shape: declarations go through
`Logger::Options` at `create()`, and `declareSubsystem()` afterwards, rather
than being announced from the emit path -- the emit path had no reason to
learn about this, and control-thread work costs a record nothing. The format
version did not move: an older decoder counts kind 8 as skipped rather than
as damage, and a test pins that instead of a comment claiming it.

### 2.3 A plugin's call sites silently log nothing -- **Bug (R4.1); made loud, not yet fixed**

The probe: one `.so` with a call site inside it, one host that creates and
binds a `Logger`, both built the same way, run twice.

| built with | plugin sees binding | records decoded |
|---|---|---|
| `-fvisibility=default` | yes | 2 (host + plugin) |
| `-fvisibility=hidden` | **no** | **1** (host only) |

`-fvisibility=hidden` is the recommended default for shared libraries and is
the *only* behaviour available on Windows, where nothing is exported from a
DLL unless it is asked for. Under it, `Logger::sActive_` -- a `static inline`
member -- is a separate object in each module, so the plugin's `active()`
returns null, every call site in the plugin is a no-op, and nothing anywhere
says so: drops zero, undecodable zero, exit status clean.

R4.1 names this exact failure ("the duplicated-singleton failure that
static-linking a logger into two modules produces") and forbids it. R4's C
ABI is scheduled for v2, which is fine -- what is not fine is that the
header-only path fails *silently* in the meantime. Two things are owed
before v2: the R9.3 counter above, so the silence ends; and a documented
statement that a plugin must reach the host through the C ABI rather than by
including the header, so nobody discovers this in production.

**Half shipped, and it is the half that stops the silence.** R9.3 and
`sub0log::unboundEmits()` now exist. Re-running the same probe, the plugin's
own module reports one unbound emit while the host reports zero --
module-scoped by necessity, since a plugin that cannot see the host's
instance cannot see its counter either, which is exactly the granularity
that makes the case legible from inside the module suffering it. The
duplicated instance itself is unchanged: R4's C ABI is still the fix and
still v2.

## 3. First month: running it in a service

### 3.1 One threshold for the whole process -- **Gap, closed**

`Logger` holds a single `std::atomic<Severity> threshold_`. Turning on debug
logging for the storage subsystem in a running service means turning it on
for everything, at whatever the aggregate rate is -- which for a library
that measures emit at 72 ns is a great deal of traffic to invite in order to
watch one component.

Per-subsystem thresholds are the universal expectation from every logger a
consumer has used before (spdlog loggers, `log4j` levels, `RUST_LOG`
filters). Given `SubsystemId` is a `uint32_t`, a small fixed-size level
array indexed by a low id -- with the global threshold as the fallback --
keeps the hot check to one or two relaxed loads and no branch on the
disabled path that is not already there. Worth measuring against the 0.71 ns
disabled-site KPI before committing to a shape.

**Shipped**, and measured rather than assumed: the table lookup costs
nothing, because a call site's id is a constant and every slot always holds
an answer -- `setThreshold(severity)` writes the whole table rather than
leaving a sentinel for the read side to resolve. Ids at or above
`cSubsystemLevels` follow the process-wide threshold, which is what keeps
the index a constant offset.

### 3.2 A full segment stays full -- **Deliberate; story now written**

Rotation is explicitly out of scope in `REQUIREMENTS.md`, and that is the
right call for a v1: rotation policy is where logging libraries go to
acquire configuration surfaces. But a long-running service *will* fill its
segment, and what happens then is that every subsequent record is dropped
and counted, forever, while the process keeps running perfectly.

The gap is not the missing feature, it is the missing paragraph. A consumer
choosing `segmentBytes_` needs to know: the segment does not wrap, drops are
permanent once full, and the intended pattern is a new segment per run (or
per interval) with the merger putting them back together at read. That is a
coherent story -- it is just not written down anywhere a consumer will find
it before they hit it.

**Written down**, in the README under "Operating it".

### 3.3 Drops are pollable, not observable -- **Gap; answered as designed**

`stats()` is the only way to learn about a drop. A service that wants to
alert on record loss has to poll it on a timer it invents. A callback is the
wrong answer (it would put consumer code on the emit path, which R1 forbids)
but a monotonically-increasing counter that a metrics scrape can read is
already what `Stats` is -- what is missing is one example showing it wired
to a gauge, and a note that reading it from another thread is safe and
cheap.

**Answered as designed, not as asked.** `examples/09_operating.cpp` shows
the metrics-thread shape and publishes all three numbers, filling a segment
on purpose so the ledger it checks is a real one. Still no callback, for the
reason above.

## 4. Reach: platforms and shapes we exclude by accident

### 4.1 Windows paths outside the active code page -- **Bug, fixed**

Two `CreateFileA` calls in `detail/platform.hpp` (an earlier version of this
file said four, having counted the two error tags that name them alongside
the calls themselves). A directory containing a non-ASCII character -- which includes any path under a user profile whose
name is not ASCII, a common case outside en-US -- fails to open, and the
Logger comes back invalid. Long paths (`\\?\` prefixed, over `MAX_PATH`)
fail for the same reason.

The fix is mechanical: `CreateFileW` with a UTF-8 to UTF-16 conversion at
the boundary. The reason to treat it as a bug rather than a gap is that the
failure lands on a class of user defined by their name, and the library's
own portability requirement (R8.1) does not carve out an ASCII-only subset
of Windows.

**Fixed**: `CreateFileW` with a checked UTF-8 to UTF-16 conversion at the
boundary. Ill-formed UTF-8 fails classified rather than reaching the API as
a mangled string, and the share modes were carried across deliberately --
retyping a call is exactly how a widened share mode gets quietly reverted.
What Windows CI still does not cover is a segment created under a real
non-ASCII directory, as opposed to the conversion in isolation.

### 4.2 Async-signal-safety is claimed by example, not by contract -- **Now a contract**

Example 04 logs from inside a signal handler and is right that the emit path
itself is safe there: no allocation, no locks, no `errno` traffic. But two
things on the way in are not: the first call on a thread initialises a
`thread_local WriterCache` (a guard variable, possibly a call into the
runtime), and `create()` runs a function-local static for the `pthread_atfork`
registration.

Neither is reachable from a handler *if* the thread has logged once before
and the Logger was created before the handler could run -- which is true of
the example and of essentially every real program. The contract is therefore
"a thread that has already emitted a record may emit from a signal handler",
and that sentence should be in the header rather than inferred from an
example.

**In the header now**, and sharper than the finding was. In an executable
the writer cache is constant-initialised into `.tbss` -- no guard variable
and no runtime call in the object file at all -- but the same access
compiled `-fPIC` into a shared library goes through `__tls_get_addr`, which
may allocate on a thread's first touch. So the contract is "a thread that
has already emitted may emit from a handler", and its reason is one that
only appears in a shared library.

## What this changed

Everything above except one item, worked in the order the list was ranked
in. The exception is the plugin's duplicated instance (2.3), where the
silence was ended and the cause was not: R4's C ABI is the fix and is
scheduled for v2.

Two claims made when the list was written turned out to be worth revising
once the work was done, and both are corrected in place above rather than
tidied away:

- There were **two** `CreateFileA` calls, not four. The original count
  included the two error tags that name them.
- The `std::string` refusal was not merely inconvenient, it was
  **misreasoned**: the hidden allocation it claimed to prevent does not
  happen, which is a stronger statement than the finding made and the reason
  it was safe to close rather than soften.

One thing the work found that the list did not, and it has already needed
correcting once. R9.3's counter is not free on the *disabled* path even
though a disabled site never takes its branch: it costs about 0.15 ns,
0.57 to 0.72 measured over eight runs each, because the branchless compare
the check used to compile to has to become a real branch for the cold call
to hang off. That is a good trade -- 0.15 ns for the end of a class of
failure where records vanish while every counter reports health -- but the
commit that introduced it claimed the cost was nothing, on a measurement
taken before the rest of this work reshaped the file.

The correction is worth more than the number. When the cold attribute went
in, the three measurements were 0.57 with no counter, 1.08 inlined, 0.56 out
of line: it appeared to recover everything, and justified an isolated
non-standard attribute under R8.2. Re-measured after the file had been
reordered, they are 0.57, 0.79 and 0.72 -- the compiler now makes a much
better job of the inlined form, and the attribute is worth about 0.07 ns.
It is kept, because that is still a tenth of R1.4's headline number and one
macro is a cheap way to hold it. But a justification is a measurement of a
particular state of the code, and a stale one left standing is exactly how
an extension outlives its reason.

The original observation still holds and is worth keeping at the end,
because it is the reason to keep doing this: nothing on the list touched the
wire format or the emit path's cost. Everything here lived in the space
between a correct library and a usable one, which no requirement was
watching -- and the way to find the next one is the same as the way this
lot were found, which is to write the code a consumer would write and look
at what actually happens.
