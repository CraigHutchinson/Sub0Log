# Debt review: rot across four axes

`api-review.md` asked whether the surface agrees with itself. This asks a
narrower, more mechanical question, axis by axis: is anything duplicated
that would have to change in two places at once; does anything type-pun
past what the object model actually grants; is any header's shape hiding
its own intent; is any comment padding or lying. Round 1.

The verdicts are the same three the other two registers use:

- **Bug** -- the code (or the doc describing it) does the wrong thing.
- **Gap** -- nothing is unsafe, but something costs more to maintain than
  it should, cheaply fixed.
- **Deliberate** -- looks like debt at a glance, checked, and is not:
  stated here so the next round does not re-open it.

Ranked by cost. **Applied** entries are done, in this tree, and
re-verified against the full suite. **Proposed** entries are this
review's opinion; the constraints on this round put a UB fix and any
signature/behaviour change out of my hands here regardless of confidence.

## 1. Two `atomic_ref<uint64_t>` were built over storage that was never given `uint64_t` lifetime

**Bug (technical UB; no evidence of real-world miscompilation), applied.**

```cpp
// chunk.hpp, ChunkWriter::commit
std::atomic_ref<std::uint64_t> headRef{
    *reinterpret_cast<std::uint64_t*>(slot.headWord_)};

// segment.hpp, Segment::claimChunk
std::atomic_ref<std::uint64_t> cursor{
    *reinterpret_cast<std::uint64_t*>(base + wire::cNextChunkOffset)};
```

Both dereferenced a `reinterpret_cast<std::uint64_t*>` into raw `mmap`/
`MapViewOfFile` storage to *form a reference*, then handed that reference
to `atomic_ref`'s constructor. `atomic_ref` does not create the object it
wraps -- per `[atomics.ref.generic]` its precondition is that "the
referenced object" already exists -- and forming `T&` by dereferencing a
`reinterpret_cast<T*>` is only defined when an object of type `T` (or one
compatible under the aliasing exceptions) already occupies that storage
([basic.life]). `mmap` is not one of the operations C++20's
implicit-object-creation rule names (`malloc`, `operator new`, `memcpy`,
and a short fixed list -- P0593); nothing in this path created a
`uint64_t` at either address before the reference was formed.

This codebase already knew the correct pattern and used it everywhere
else: `wire::loadUnaligned`/`storeUnaligned` are `memcpy`-based precisely
*because* `memcpy` is one of the operations that does implicitly create an
object at its destination, so a raw byte buffer can be read or written as
any trivially-copyable `T` without this question ever coming up. The
reader side of this exact head word (`reader.hpp`, `SegmentReader::visit`)
goes one step further and avoids `atomic_ref` entirely -- it
`loadUnaligned`s the word and pairs it with a separate
`std::atomic_thread_fence(std::memory_order_acquire)`, a choice its own
comment explains and measures. The two producer-side sites above were the
only place in the library that constructed `atomic_ref` by dereferencing a
raw pointer into mapped memory instead.

**The fix that shipped is not the one first proposed, and the difference
matters.** `std::start_lifetime_as<std::uint64_t>(p)` is the standard's
answer -- it implicitly creates the object and returns a valid `T*` -- but
it is not yet what this project's own compilers actually ship: absent
from `<version>`'s `__cpp_lib_start_lifetime_as` and from a direct compile
probe on both GCC 13 and Clang 18, the exact toolchains `linux-gcc` and
`linux-clang` build against in CI. A source change that "changes no
instruction on any mainstream compiler" is only true of compilers that
have the function; applying the originally-proposed snippet unconditionally
would have broken the build it was meant to make safer.

What shipped instead is `wire::startUint64LifetimeAt` (`wire.hpp`), used
at both sites: `std::start_lifetime_as` when
`__cpp_lib_start_lifetime_as >= 202207L` is defined, the original
`reinterpret_cast` otherwise. The fallback is unsound by the same strict
reading, not by anything narrower -- but it is what every mainstream
compiler has always done with `mmap`-obtained storage in practice, so
shipping it as the fallback trades nothing away on the compilers that
lack the real fix and removes the gap on any that gain it. Verified
compiling and passing 91/91 on both the `<uint64_t*>`-fallback path
(GCC 13, Clang 18, locally) and re-run clean under ASan+UBSan -- a
sanitizer catching nothing is not proof the object model is satisfied,
but it is evidence the fallback still behaves identically to what it
replaced.

Confidence: high that the original code was UB under a strict reading of
the object model; high that the shipped fix removes it wherever the
standard library provides the tool, and changes no behaviour wherever it
does not.

Alignment itself was always sound and was checked separately:
`Segment::create` refuses any `chunkBytes_` that is not a multiple of
`wire::cRecordAlign` (8), `cNextChunkOffset` is a compile-time constant
`128`, and `mmap`/`MapViewOfFile` bases are always page-aligned -- so both
addresses are genuinely 8-aligned on every path; the only defect here was
the missing lifetime step, never a real misalignment.

## 2. `docs/test-plan.md`'s "What runs where" predates most of v2

**Bug (stale, self-contradicting numbers), applied.**

Three claims in the file, checked against the actual test sources and a
real `ctest` run:

| claim | said | actual |
|---|---|---|
| Linux/macOS unit+integration+system | 17 + 22 + 14 = **53** | 25 + 36 + 29 = **90** (measured: `ctest --test-dir build`) |
| Windows unit+integration+system | 17 + 22 + 2 = **41** | 29 + 36 + 21 = **86** (derived from source: see below) |
| `example` label | "8 programs" | 12 (`ctest -L example`: `example::01_hello` .. `example::12_plugin_abi`) |
| which examples are Windows-excluded | "04, 05, 06" | 04, 05 only -- `examples/CMakeLists.txt`'s own comment says why: 06 (and 10) spawn through `ChildProcess::spawn()`, which has had a real `CreateProcessW` arm since v2 |
| which `system` tests Windows skips | "the nine child-capture tests" (implying all of them) | 2 of the 10 (`tests/system/child.test.cpp`'s POSIX-signal case and bad-executable-path case) -- confirmed by reading every `#if defined(_WIN32)`/`#if !defined(_WIN32)` guard in the file; the other 8 are wrapped in nothing and run everywhere |

The same document elsewhere (`docs/architecture.md`, and
`examples/06_child_capture.cpp`'s own file comment, which cites it) already
said the true shape correctly -- "seven of the nine... the two exceptions
[are a signal case and a bad-path case]" -- which is *closer* to right than
test-plan.md's "all nine" but is itself off by one against the current
test file (a ninth and tenth spawning `TEST_CASE` -- the R9.1/R9.2 line-cap
and no-logger-bound tests -- were added later without the count being
revisited): the real split is 8 of 10, not 7 of 9.

Two different staleness mechanisms are visible here, not one: test-plan.md
predates the entire v2 Windows child-capture arm (it still describes
child capture as wholly POSIX-only, contradicting `architecture.md`'s own
"v2 (complete)" entry two files over), while `architecture.md`/example 06
are merely one test short of current. Both are evidence for the same
underlying lesson -- a claim about test topology drifts the moment a test
is added, and nothing catches it because it is prose, not an assertion.

**Applied:**
- `docs/architecture.md` and `examples/06_child_capture.cpp`: "seven of
  the nine" -> "eight of the ten" (both cite the same fact; both were one
  test short).
- `docs/test-plan.md`: the "What runs where" table, the `example` program
  count, the POSIX-only example list, the unit/integration "run
  identically everywhere" claim (unit does not -- `platform.test.cpp` is
  four Windows-only cases for `toWidePath()`), the child-capture exclusion
  count, and both later restatements of the 53/41 totals (`R8.1 three
  platforms` in the requirements table, and the "Known gaps" section).
  Linux/macOS numbers are measured directly against this tree; the Windows
  column is derived by reading every platform guard in
  `tests/unit/`, `tests/integration/`, and `tests/system/` (no Windows
  arm was actually run) -- stated as such in the doc itself, with a note
  to confirm it against real CI rather than trust it silently the way the
  numbers it replaces were trusted.

## 3. Child* payload hand-decoding is duplicated between an example and a test -- checked, deliberate

**Deliberate; no action.**

`examples/06_child_capture.cpp` and `tests/system/child.test.cpp` each
define their own `RawRecord`/`readAllRecords`-shaped walk over
`SegmentReader::visit()` and their own offset arithmetic against
`wire::ChildStartPayload`/`ChildOutputPayload`/`ChildExitPayload`
(`loadUnaligned<ChildStartPayload>`, then a `u16` length, then the tail
bytes, repeated per kind). This is real, verified duplication -- not
superficially similar, the byte-offset arithmetic is identical -- but both
copies exist for a reason the example's own file comment states before
this review went looking: `Decoder::decodeAll()` has no shape for the
three `Child*` kinds by design (`reader.hpp` counts them
`skippedRecords()` rather than decoding them, since a captured child has
no call sites for a site table to be built from), so anything that wants
typed access to them has no library entry point to call, only the wire
struct definitions to read against. The example needs its own copy because
it must read as standalone (the same reasoning that makes `slurp`/
`onlySegment` duplicated across all eleven numbered examples deliberate --
see "What was checked" below); the test needs its own because test support
that decoded the library's own record kinds for it would be testing the
support code, not the library.

A library-side convenience -- a free function pairing each `Child*Payload`
struct with its trailing bytes, so the offset arithmetic exists once --
would remove the duplication without touching `Decoder`'s scope, but it is
a new piece of public surface (a signature this round's constraints put
out of reach), and the two existing copies are twelve lines apiece with a
`REQUIRE`/bounds difference between them that reflects their different
jobs (the test checks malformed input paths the example does not need to).
Noting it rather than proposing it: it is real but small, and the design
question ("should `wire.hpp` or `reader.hpp` grow a Child*-decode helper")
is exactly the kind of API decision `api-review.md` already defers rather
than a debt-register call.

## 4. `emitChained` (`detail/emit.hpp`, ~175 lines) -- size is mostly deserved

**Deliberate; noted, not actioned.**

`detail/emit.hpp` is the largest header by line count (632) and
`emitChained` is its longest function, reached only when a call site's
argument pack contains a `Bytes`-shaped argument. Read start to finish it
does five distinct things in sequence: scan every argument once for its
true length; split each oversized argument into `ArgOverflow` slices;
reserve the whole chain atomically with a three-tier fallback (current
chunk, a freshly claimed one, then flat truncation if even an empty chunk
cannot hold it); write the message and every continuation payload; and
commit tail-to-head so a reader can never observe a half-published chain.

That is genuine complexity, not narrated complexity: every phase is load
bearing (the all-or-nothing reservation and the reversed commit order both
exist because an earlier, simpler version of this code was measurably
wrong -- `reserveChain`'s own comment tells that story), and the function
is exercised by `benchmarks/emit.bench.cpp` specifically to keep the
`if constexpr` split from taxing the plain (non-`Bytes`) call sites that
never instantiate it. A closer look for an unjustified fallback path
(named in this round's brief as something to check) found the opposite of
one: the third tier (flat truncation) is the *same* behaviour a
`Bytes` argument always had before continuation chains existed, reused
rather than duplicated, and `wire.hpp`'s `cFlagTruncated` keeps meaning
what it always meant across all three tiers.

Where it could shrink without changing behaviour: the "scan and size" loop
(lines ~379-426) and the "build the overflow list" loop (~428-438) thread
seven local arrays/counters between them and could be one private helper
returning a small struct, at the cost of one more call and one more type
to name in a file that is already dense. Worth doing in some future round
if the function grows again; not urgent, and not attempted here because
"restructure the internals of a producer-path function" is a signature-
adjacent, behaviour-risking change this round's constraints keep out of
reach regardless of how mechanical it looks.

`include/sub0log/child.hpp` (1192 lines, the largest file) reads larger
than it compiles: from `captureLoop`/`spawn` onward it is one `#if
!defined(_WIN32) ... #else ... #endif` block, so any single translation
unit only ever sees roughly 600 of those lines. No responsibility was
found pointing the wrong way -- `grep`ing every other header for
`ChildProcess`/`child.hpp` found nothing but one doc-comment mention in
`instance.hpp` (already the subject of `api-review.md` #6); the wire
format and the decoder both stay ignorant of `ChildProcess` by design.

## What was checked and found sound

**DRY.** The three candidates named in this round's brief, verified rather
than assumed:
- **`slurp`** (read-a-segment-into-bytes) is reimplemented in all twelve
  example programs (01-11 plus `plugin_abi/host.cpp`), `tools/sub0log_cat.cpp`,
  and twice more under `benchmarks/` (as `slurpFile`) -- fifteen copies --
  versus one shared `sub0log::test::slurp` used by every test. Deliberate
  on both sides: `tests/support/fixtures.hpp`'s own file comment explains
  the shared one, and every example's header comment ("what this teaches")
  frames each file as a single-file tutorial meant to be read top to
  bottom on its own -- pulling `slurp` into a shared example-support
  header would break exactly that property for a six-line function. Two of
  the fifteen copies (`examples/09_operating.cpp`, `tools/sub0log_cat.cpp`)
  write bytes with a `memcpy`/cast loop instead of the
  `reinterpret_cast<const std::byte*>` + range-construct the other thirteen
  use; both are legal (the char/byte aliasing exception applies either
  way) and functionally identical, so this is inconsistency, not a bug --
  not worth a finding of its own.
- **"open, decode, walk the records"** across examples and tests is
  exactly-as-similar as calling any library API the same way twice: every
  site is `SegmentReader::open` + `Decoder::decodeAll` + `Decoder::format`,
  with no logic of its own to duplicate. Not a finding.
- **Hand-decoded `Child*` payloads** -- real duplication, deliberate; see
  finding 3 above.
- `detail::reserveRecord` (`instance.hpp`) is the one "try the current
  chunk, refill, count the drop" helper, reused as-is by `emit()`,
  `abi_host.hpp`'s ABI `emit`, and three record-writing paths in
  `child.hpp` -- correctly *not* reused by `emitChained`/
  `writeSiteDefinitionCore`, which need a fallback shape `reserveRecord`'s
  single-reservation contract cannot express. This is the shared-helper
  pattern working as intended, not a gap.
- `Merger` composes `Decoder` rather than re-implementing any of its
  decode logic; `tools/sub0log_cat.cpp` is a thin consumer of `Merger`/
  `Decoder`, nothing more.

**Aliasing and UB**, beyond finding 1: `wire::loadUnaligned`/
`storeUnaligned` are `memcpy`-based and sound (memcpy is a recognised
implicit-object-creation operation, so no lifetime question arises even
though the destination is raw mapped memory). Every `reinterpret_cast` to
`const char*` over a `std::span<const std::byte>` (`reader.hpp`, for
`string_view` construction) is covered by the standard's char/
`unsigned char`/`std::byte` aliasing exception. `site.hpp`'s
`reinterpret_cast<std::uint64_t>(this)` is an ordinary (implementation-
defined, not undefined) pointer-to-integer cast. `encode.hpp`'s pointer
handling does the same, and its zero-extension of a 32-bit pointer to the
wire's fixed 8-byte `Pointer` width was already reviewed in
`adoption-friction.md`. `abi_host.hpp`'s `SiteAnnounceTable` uses
`std::atomic<std::uint64_t>` as ordinary struct members inside a normally
constructed `std::array` -- real objects with real lifetime, no aliasing
question at all. `reader.hpp`'s `std::atomic_thread_fence(acquire)` paired
with a plain (non-atomic) `loadUnaligned` of the head word is a known,
narrower theoretical gap (a fence formally orders atomic operations, not
plain loads) but it is already deliberate and measured in its own comment,
in the same "Deliberate, and right" shape `api-review.md` #11 uses, and
was not reopened here.

**Clarity of design.** No header found doing a job that belongs to
another: the wire format, the decoder, and `child.hpp` each stay ignorant
of the others' internals (checked by grepping every header for the other
layers' type names). `emit.hpp` and `child.hpp`'s sizes are addressed
above; both are symptoms of genuine complexity (a chain-aware write path;
two full platform arms plus shared record-writing code), not of an
unclear shape.

**Comment bloat and staleness.** Every block of 12+ consecutive comment
lines across all sixteen headers was located and read (`chunk.hpp`,
`version.hpp`, `child.hpp` x3, `wire.hpp` x4, `merge.hpp`, `encode.hpp`
x4, `abi_host.hpp` x3, `instance.hpp` x7, `reader.hpp` x3, `site.hpp`,
`log.hpp`, `segment.hpp`, `detail/platform.hpp` x4, `detail/emit.hpp` x5 --
including the single largest block in the codebase, 53 lines at
`reader.hpp:173`, `Decoder`'s lifetime paragraph). None were bloat by the
house test (does it record a decision, a measurement, a correction, or a
hazard): every one ties to a `docs/*.md` decision, a benchmark number, a
requirement id, or -- repeatedly -- an earlier wrong design and what
proved it wrong (`reader.hpp`'s `std::deque<Loaded>`-vs-`std::vector`
ASan story, `wire.hpp`'s corrected `cMaxContinuations` arithmetic,
`site.hpp`'s "a plain flag would be wrong, and was", `instance.hpp`'s
restated R9.3 cost after an earlier measurement undersold it). High
comment share in this codebase measures density, not narration -- no
header-comment cuts were made this round because none were found to
justify one. The four smallest, highest-percentage files
(`version.hpp`, `site.hpp`, `severity.hpp`, `log.hpp`) were read in full
rather than sampled, for the same reason: less surface to hide bloat in
is also less surface where a five-minute read fails to be conclusive.

Stale claims beyond the two applied above: none found. Every absolute
claim searched for ("does not", "never", "always", "not supported", plus
every hardcoded count) that was checkable against current code or a real
build checked out true.

## Estimate for round 2

Thin. This tree has already been through `api-review.md` and
`adoption-friction.md`, both of which read the same sixteen headers hard;
this round's yield reflects that -- one real (if low-risk) UB gap, since
applied; one doc-staleness cluster, applied; and two confirmed-sound
"looks like debt, isn't" write-ups, out of four axes and sixteen headers
read start to finish. What is left for a human decision rather than a
round 2 pass: whether a `Child*`-decode helper is worth adding to the
public surface (finding 3). A round 2 would mostly be re-confirming
Windows numbers against a real CI run rather than the source-derived ones
landed here, and re-reading whichever headers change next for new bloat
-- not re-auditing what this round already covered.

**Update, post-review:** finding 1 was taken. Not the exact snippet
proposed, though -- `std::start_lifetime_as` turned out to be absent from
both compilers this project's own CI actually builds with (GCC 13,
Clang 18; confirmed by a direct compile probe, not by reading a
cppreference table), so the fix that shipped is a feature-tested helper
(`wire::startUint64LifetimeAt`) that uses the real operation where the
standard library has it and falls back to the original `reinterpret_cast`
where it does not. Applying a "compiles to nothing beyond the
`reinterpret_cast` it replaces" claim without first compiling it against
the project's stated floor would have been the same mistake this file
spent its second half hunting for elsewhere: an absolute claim nobody
checked.
