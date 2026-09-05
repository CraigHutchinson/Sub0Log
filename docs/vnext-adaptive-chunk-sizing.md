# vNext: adaptive chunk sizing

A sibling to `vnext-segment-rollover.md`, not a layer of it. Rollover
stayed entirely at file granularity and never touched the wire format;
this does, because the problem it addresses -- a thread's chunk claim
being permanently the wrong size for how much that thread actually logs
(`docs/memory.md`, "A thread costs a whole chunk, permanently") -- cannot
be fixed at file granularity at all. Split into phases on purpose. Phase 1
(below) is implemented and merged, and deliberately changed nothing
observable -- it put the self-describing wire format through real use
while every chunk in a segment stayed the same size, exactly as before.
Phase 1.5, also implemented, makes the reader genuinely self-describing:
it can now correctly decode a segment whose chunks actually differ in
size. What remains -- an allocator that ever produces such a segment, and
the actual per-thread growth/shrink policy -- is Phase 2, and is not
built.

## Why this is a wire-format change and rollover wasn't

`SegmentReader::visit()` used to stride a whole segment by one constant,
`header_.chunkBytes_`, read once from `SegmentHeader` -- a segment-wide
value, not a per-chunk one. For different chunks to legitimately be
different sizes, *something* has to say how big each one actually is,
and the only place that can live is the chunk itself. That is a change to
`wire::ChunkHeader`, which makes it a wire-format question in a way
nothing in the rollover document ever was.

## Phase 1: self-describing, but still uniform -- shipped

The brief this phase was built to: add the mechanism, prove it against
real reads and writes, and change nothing about what a segment actually
looks like today. Every chunk in a segment is still exactly the same
size, chosen once, exactly as before `Segment::create()` returns. What's
different is that the size is now *stamped and checked* per chunk,
not only assumed from one segment-wide field.

**The encoding.** `wire::ChunkHeader` gained one byte,
`chunkSizeClass_` (`wire.hpp:303`), out of what used to be eight bytes of
silent reserved space -- seven of those eight bytes stay reserved.
Class 0 means "unspecified, defer to `SegmentHeader::chunkBytes_`"; a
real class `k` in `[1, cMaxChunkSizeClass]` means `cChunkSizeUnit << (k -
1)` bytes (`wire.hpp:101`), with the reverse lookup
(`sizeClassForChunkBytes`, `wire.hpp:112`) used once, at
`Segment::create()`, to turn a caller's `chunkBytes_` into a class.

**Why class 0 is the sentinel, and why that means no format-version
bump.** `wire::cFormatVersion`'s own comment (`wire.hpp`, right above the
constant) already argues this exact case: version gates *everything*,
so bump it only when an old reader would otherwise be misled, never as a
reflex for "the struct changed." Every chunk this format ever wrote
before this field existed has that byte at 0, because it was silent,
always-zero reserved space -- which is exactly what "unspecified" means.
An old reader still never looks at it (unchanged). A new reader sees an
old chunk's 0 and defers to `chunkBytes_`, which is exactly what it
would have done anyway. Neither direction needs a version gate, so
`cFormatVersion` stays exactly where it was. This was checked against the
actual test-builder code before relying on it, not assumed:
`tests/support/segment_image.hpp`'s `wire::ChunkHeader ch{generation,
ownerThread, claimMonoNs, 0}` -- the aggregate-init call every
pre-existing reader test already used -- still compiles unchanged and
still produces class 0, because `0` lands on the new
`chunkSizeClass_` and the trailing `reserved_[7]` value-initialises with
no explicit initialiser. Nothing needed touching for old code to keep
meaning exactly what it always meant.

**Why the unit is 128, not the 4096 first floated for this design.**
Checked against real configurations before picking a number: three
existing tests deliberately configure chunks smaller than 4096 --
`tests/integration/producer.test.cpp`'s `128u` ("minimum-viable: fits a
few small records") and `256u`, and `tests/integration/continuation.test.cpp`'s
`1024u` ("smaller than a full chain needs", specifically testing the
boundary at `cInlineBytesCap * (1 + cMaxContinuations)` = 4096 -- a
segment quantised to multiples of 4096 could not express "smaller than
4096" at all, breaking that test's actual point). 128 as the base unit,
scaled by powers of two, reproduces every chunk size this codebase
configures anywhere today exactly: 128, 256, 1024, 4096, 16384 and
`cDefaultChunkBytes` (65536) are all `128 << k` for some k
(`tests/unit/wire.test.cpp`, "chunk size classes round-trip and cover
every size this codebase actually uses" -- the list in that test is the
proof, not an assertion). The ceiling, `cMaxChunkSizeClass = 17`
(`wire.hpp:94`), is `128 << 16` = exactly `cDefaultSegmentBytes` (8 MiB) --
a chunk larger than the whole default segment would leave room for at
most one, so that's a meaningful ceiling to pick rather than an arbitrary
one, and the field itself (a full byte) can represent far more if that
default ever changes.

**A configured size that isn't representable is not rejected.**
`Segment::create()` (`segment.hpp:151`) computes
`sizeClassForChunkBytes(chunkBytes).value_or(0u)` -- best-effort, never a
new way to fail. A `chunkBytes_` that happens not to be `128 << k` for
any k just gets class 0, exactly the legacy behaviour, with no new
validation failure introduced. Every value this codebase actually
configures today is representable, but nothing requires a caller's
choice to be, which matches this format's habit of degrading rather than
refusing wherever a degraded answer is still an honest one (R3.3's whole
"read what you can, count what you cannot").

**What Phase 1's reader did with it.** `SegmentReader::visit()` still
strode by `header_.chunkBytes_` unchanged -- that phase was validation,
not navigation. When a claimed chunk's stamped class was non-zero, it was
cross-checked against `header_.chunkBytes_`; agreement cost nothing
extra, a mismatch was counted as damage. A segment whose chunks actually
differed in size could not be read correctly at all. That was deliberate
scope, not a limitation left standing -- Phase 1.5 replaces it.

**Proof, not assertion, for Phase 1.** `tests/unit/wire.test.cpp` covers
the encoding in isolation (round-trip across every size this codebase
uses, the unrepresentable case, the ceiling, the sentinel default).

## Phase 1.5: the reader becomes genuinely self-describing -- shipped

`SegmentReader::visit()` (`reader.hpp`) no longer treats
`header_.chunkBytes_` as ground truth for a claimed chunk. For a chunk
whose `generation_` matches the segment's own, its size is now: its own
stamped `chunkSizeClass_` if that's non-zero *and* no greater than
`wire::cMaxChunkSizeClass`; `header_.chunkBytes_` otherwise (the class-0
sentinel, or a best-effort-unrepresentable size from `Segment::create()`
-- both mean the same "defer to the segment default" as before). Phase
1's cross-check -- flagging a differently-sized chunk as damage -- is
gone; a differently-sized chunk is now legitimately different, not wrong.

**The bounds-check this needed, found before it was written.**
`chunkSizeClass_` is read from an untrusted image -- once navigation
*trusts* it rather than only cross-checking it, an out-of-range value
(anything past `cMaxChunkSizeClass`, which nothing this format ever
writes stamps) would otherwise decode into a wildly wrong stride, or
worse: `chunkBytesForSizeClass()`'s own precondition (`sizeClass` in
`[1, cMaxChunkSizeClass]`) means a class of, say, 200 shifts a 32-bit
value by 199 bits -- undefined behaviour, not merely a wrong answer.
Caught before writing the navigation change by asking what happens to an
adversarial or corrupted value, not discovered by it happening: a class
past the ceiling is treated exactly like a torn record already is
(`RecordHead::payloadBytes_` bounds-checked before use, right below this
in the same function) -- damage, decoding stopped, never turned into a
byte count that steers anything. Tested directly (`tests/integration/
reader.test.cpp`, "a stamped size class past the format's own ceiling is
damage, never decoded into a stride").

**Unclaimed space still uses the segment-wide default, and that remains
correct, not merely convenient.** A never-claimed chunk (`generation_ ==
0`) has no stamped size -- nobody wrote a real header there -- so it is
still stepped through at `header_.chunkBytes_`, one slot at a time,
exactly as before. For every allocator that exists today (uniform claims
only), this is exact. For a future allocator whose claims vary (Phase
2's still-unbuilt half), it stays *correct* though not maximally
efficient: such an allocator can only ever leave unclaimed space as a
single trailing run (nothing is ever claimed past where its own cursor
stopped), so this loop simply keeps re-finding `generation_ == 0` at
each `header_.chunkBytes_`-sized step until it reaches the segment's own
end, counting all of it as unwritten -- more iterations than the
single-arithmetic-step shortcut originally sketched for this problem
would take, but never a wrong answer, and never at risk of stepping over
real data hiding in the gap, because a byte-offset-cursor allocator
never leaves one there to step over. That shortcut remains available as
a later optimisation; it was never a correctness requirement, which is
why Phase 1.5 shipped without it.

**Proof, not assertion, for Phase 1.5.** `tests/integration/reader.test.cpp`:
a chunk stamped smaller than the segment default is read as that smaller
chunk, record and all, with the true remainder correctly counted as
unwritten rather than flagged as damage; the out-of-range-class case
above; and -- the actual point -- two chunks of genuinely different
sizes (128 and 512 bytes), laid out back to back the way a variable-size
allocator would place them, hand-built at the byte level and decoded
correctly in one pass, in order, each found starting exactly where the
one before it actually ended rather than where a fixed stride would have
guessed. 102/102 tests in the suite (100 before this change, 1 rewritten
because it tested the now-superseded cross-check, 3 added), clean under
a plain build and under ASan+UBSan both.

## Phase 2: an allocator that varies, and the actual policy -- not built

This is what "adaptive chunk sizing" was asked for in the first place.
The reader can now correctly decode whatever such an allocator produces;
producing it, and deciding what size to ask for, is still exactly the
shape argued through in the conversation this document is written up
from, unimplemented:

- A thread's **first** claim in a fresh segment starts small.
- **Growth** on repeated exhaustion: a thread that fills its chunk and
  needs another asks for a bigger one next time -- the closest real
  precedent is thread-caching allocators (tcmalloc, jemalloc) growing a
  thread's own size class on repeated exhaustion, not amortized-vector
  doubling, because it's specifically about per-thread sizing under
  concurrency, which is the same problem here.
- **Shrinking** across a segment boundary: a thread whose chunk survived
  to end-of-segment (rollover, `vnext-segment-rollover.md`) without
  filling is decent evidence it logs lightly, worth starting smaller
  next time.
- Both directions only ever change what a thread asks for on its *next*
  claim -- never touching a chunk already handed out, which is the same
  distinction that rules out reclaiming a chunk mid-life at all
  (`docs/memory.md`: `wire::ChunkHeader::ownerThread_` is stamped once,
  per chunk, so a second thread resuming one would misattribute its
  records to the first).

**What Phases 1 and 1.5 already resolved, so Phase 2 does not have to
re-derive any of it:** the encoding, the sentinel-based backward
compatibility, the unit/ceiling choice, and -- as of Phase 1.5 -- a
reader that already decodes a genuinely variable-size segment correctly,
including its adversarial edges (an out-of-range stamped class, an
unclaimed stretch that isn't stride-aligned). What Phase 2 still needs,
precisely, is now only the *producer* side:

- **An allocator whose claims actually vary.** `Segment::claimChunk()`
  today claims exactly `chunkBytes_` every time, via `fetch_add(1)` on an
  index cursor scaled by that one fixed size. Letting a claim's size vary
  means the cursor has to track cumulative *bytes* requested rather than
  a *count* of uniform units -- a `fetch_add(requestedBytes)` returning
  the offset before the add, checked afterward against the segment's
  true remaining space, is the natural shape and needs no CAS: an
  overshooting claim simply marks the segment exhausted for everyone
  after it, exactly as an index past `chunkCount_` already does today --
  there is no free list to give a smaller, later request access to
  whatever the overshoot left behind, so nothing is lost by not trying to
  recover it.
- **Where a thread's running size history lives, and that it survives
  rollover rather than resetting with it.** The natural home is the same
  `thread_local WriterCache` (`instance.hpp`) already used for the
  current chunk -- but everything in that struct today resets on a
  generation/owner change (`currentWriter()`'s `cache.owner_ != this`
  check), which is exactly the moment a rollover happens. A size-history
  field has to be the one thing in that cache that *survives* the swap
  rather than resetting with it -- an easy change to make deliberately,
  an easy bug to introduce by accident if bundled naively into the
  existing invalidation path. Named here so it is a decision when Phase
  2 is built, not a discovery.

## What this does not change

`Segment::create()` still produces byte-for-byte the same segment for
the same `SegmentOptions` as before any of this work -- Phase 1.5 changed
what the reader is *able* to decode, not what the producer actually
writes by default, and every value this codebase has ever configured
anywhere continues to work without modification. R1.3 (no lock on the
producer path) is untouched -- `claimChunk()` stamps an already-validated
class, never computes one on the emit path, and nothing in Phase 1.5
touches the producer at all. R3.1/R3.2 are untouched -- nothing here
changes when or how a record becomes durable, only how a reader locates
one. `REQUIREMENTS.md`'s "Explicitly out of scope" stance on rotation
policy is unaffected, same as `vnext-segment-rollover.md`; this document
is about allocation granularity, a different axis entirely.

## Status

Phase 1 and Phase 1.5: implemented, tested (102/102, including
ASan+UBSan), merged. Phase 2 (an allocator that actually varies claim
sizes, and the growth/shrink policy deciding what to ask for): argued
through in this document and in the conversation it formalises, not
built. What used to be Phase 2's two open design questions -- how a
self-describing reader walks past unclaimed space, and where a thread's
size history lives across a rollover boundary -- are down to one: the
navigation question is answered in code, not just in this document
anymore; only the thread-history question remains for whenever an
allocator exists to need it.
