# vNext: adaptive chunk sizing

A sibling to `vnext-segment-rollover.md`, not a layer of it. Rollover
stayed entirely at file granularity and never touched the wire format;
this does, because the problem it addresses -- a thread's chunk claim
being permanently the wrong size for how much that thread actually logs
(`docs/memory.md`, "A thread costs a whole chunk, permanently") -- cannot
be fixed at file granularity at all. Split into two phases on purpose:
Phase 1 (below) is implemented and merged, and deliberately does nothing
new observable -- it exists to put the self-describing wire format
through real use before anything depends on it varying. Phase 2 is the
actual adaptive policy, and is not built.

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

**What the reader actually does with it.** `SegmentReader::visit()`
(`reader.hpp:414`) still strides by `header_.chunkBytes_`, unchanged --
this phase is validation, not navigation. When a claimed chunk's stamped
class is non-zero, it's cross-checked against `header_.chunkBytes_`;
agreement costs nothing extra, and a mismatch is counted as damage
(R9.2: more evidence forward on a failure this reader cannot otherwise
classify, not less) rather than trusted or silently ignored. A segment
whose chunks actually differ in size cannot be read correctly by this
phase's reader at all -- that is Phase 2's problem, not this one's, and
is why Phase 1 changes nothing about what `Segment::create()` produces
by default.

**Proof, not assertion.** `tests/unit/wire.test.cpp` covers the encoding
in isolation (round-trip across every size this codebase uses, the
unrepresentable case, the ceiling, the sentinel default).
`tests/integration/reader.test.cpp` covers the reader's cross-check
against hand-built segment images: agreement, the unspecified sentinel
explicitly (not left as an implicit consequence of every other test
still passing), and a genuine mismatch counted as damage rather than
decoded. All 100 tests in the suite (93 before this phase, 7 added by
it) pass, including every existing test that configures a non-default
chunk size -- 128, 256, 1024, 4096, 16384 bytes -- none of which needed
to change. Checked clean under ASan+UBSan as well as a plain build, not
only the plain one.

## Phase 2: the actual adaptive policy -- not built

This is what "adaptive chunk sizing" was asked for in the first place,
and it is still exactly the shape argued through in the conversation
this document is written up from, unimplemented:

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

**What Phase 1 already resolved for this phase, so Phase 2 does not have
to re-derive it:** the encoding, the sentinel-based backward
compatibility, and the unit/ceiling choice are all already settled and
tested. What Phase 2 still needs, precisely:

- **The reader's walk has to stop trusting `header_.chunkBytes_` for
  navigation and become genuinely self-describing** -- stride by each
  claimed chunk's own decoded size rather than a segment-wide constant.
  Phase 1 deliberately did not build this, because it interacts with a
  real, previously-unresolved question:
- **How does a reader walk past a stretch of never-claimed space when
  chunks vary in size, given an unclaimed region carries no stamped
  size at all (`generation_ == 0`, always-zero bytes)?** Resolved by an
  observation about the allocator this phase would pair with rather than
  by inventing new bookkeeping: if `Segment::claimChunk()`'s cursor
  becomes a byte-offset `fetch_add(requestedBytes)` rather than an
  index `fetch_add(1)` scaled by a fixed size (necessary anyway, to let
  claims actually vary in size), claimed regions are laid out in
  strictly increasing byte-offset order with no gaps and no unclaimed
  region ever sandwiched between two claimed ones. An unclaimed region
  can only ever be the single, contiguous *suffix* of the segment -- the
  first chunk a reader finds with `generation_ == 0` marks the end of
  all real data, and everything after it is countable in one arithmetic
  step, exactly like this reader already handles a physically truncated
  image (`reader.hpp`'s existing "everything from here to the declared
  end of the segment is physically absent" case). No new mechanism
  needed for this; the existing truncated-tail handling already is that
  mechanism, once the allocator guarantees the ordering.
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

Everything Phase 1 shipped is additive and default-preserving:
`Segment::create()` produces byte-for-byte the same segment for the same
`SegmentOptions` as before this work, `SegmentReader::visit()`'s
navigation is untouched, and every value this codebase has ever
configured anywhere continues to work without modification. R1.3 (no
lock on the producer path) is untouched -- `claimChunk()` stamps an
already-validated class, never computes one on the emit path. R3.1/R3.2
are untouched -- nothing here changes when or how a record becomes
durable, only how its chunk's size is recorded. `REQUIREMENTS.md`'s
"Explicitly out of scope" stance on rotation policy is unaffected, same
as `vnext-segment-rollover.md`; this document is about allocation
granularity, a different axis entirely.

## Status

Phase 1: implemented, tested (100/100, including ASan+UBSan), merged.
Phase 2: argued through in this document and in the conversation it
formalises, not built. The two open questions Phase 1 could not resolve
in advance -- self-describing navigation past unclaimed space, and where
a thread's size history lives across a rollover boundary -- both have
answers recorded above rather than left for Phase 2 to rediscover.
