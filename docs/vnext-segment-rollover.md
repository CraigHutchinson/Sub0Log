# vNext: segment rollover

`docs/architecture.md`'s phasing has carried this as a one-line placeholder
since it was written -- "v3: segment rollover" -- with nothing behind it.
This is that argument, worked through the way every other design decision
in this project has been: against prior art, against the actual code, and
against what would break before anything gets built. Nothing here is
implemented. The purpose is the same as `vnext-frontend-backend.md`'s: fix
where the seam goes so the eventual work is a build, not an invention.

## The problem, stated precisely

Two things, related but distinct, both already written down elsewhere in
this project rather than discovered here:

1. **A segment does not wrap** (`README.md`, "Known limitations";
   `REQUIREMENTS.md` puts rotation policy explicitly out of scope). Once
   `segmentBytes_` is spent, every later record is dropped and counted,
   permanently, while the process keeps running. The documented mitigation
   today is manual: create a new `Logger` yourself, `Merger` stitches the
   segments back together at read time.
2. **A thread's chunk claim is never reclaimed** (`docs/memory.md`, "A
   thread costs a whole chunk, permanently"). A process with many
   short-lived, lightly-logging threads exhausts a segment's chunk supply
   far sooner than its actual record volume would suggest, and nothing
   inside one segment's lifetime can get that space back --
   `wire::ChunkHeader::ownerThread_` stamps thread identity once, per
   chunk, and handing a partially-used chunk to a second thread would
   silently misattribute its records to the first (`docs/memory.md`
   again, argued in full there).

Both problems have the same shape: the fix is not inside a live segment.
It is *how many segments exist over time, and what happens to the old
ones* -- which is exactly what "segment rollover" was always going to
mean, and exactly the boundary `REQUIREMENTS.md` already drew around
"rotation policy" as something the library does not decide for you. This
document does not propose changing that boundary. It designs the
optional, caller-side layer that sits on top of it, using primitives that
already exist.

## Prior art

**RocksDB's recycled log files are already this project's own precedent**
-- `docs/framing-and-recovery.md`, "Mistake three: recycling storage
without a generation," cites `recycle_log_file_num` and RocksDB's answer:
stamp a generation on the *block*, not the record, and reject a block
whose generation doesn't match the stream's, so bytes left over from a
recycled file's previous life read as damage rather than as live data.
That reasoning is where R3.4 comes from, and it is not a citation this
project needs to go re-derive -- `Segment::create()` (`segment.hpp:94`)
already calls `randomGeneration()` fresh on every call, and
`SegmentReader::visit()` (`reader.hpp:404`) already rejects a chunk whose
`generation_` doesn't match the segment's. **This means reusing a retired
segment file's on-disk allocation for the next segment -- rather than
deleting it and paying for a fresh `ftruncate` and zero-fill -- is already
safe with the wire format exactly as it stands today.** Nothing about
R3.4 was written with rollover in mind; it turns out to already be the
right defense for it, which is the kind of thing worth confirming by
reading the code rather than assuming from the shape of the citation.

**Kafka's active/closed segment split is the closest match for the
rollover mechanism itself.** Only the active segment accepts new records;
rolling closes it read-only and opens a new active segment in its place;
retention/cleanup runs periodically (`log.retention.check.interval.ms`,
5 minutes by default) and only ever considers *closed* segments for
deletion -- the active one is never a candidate. (Kafka's own
documentation site was not reachable to fetch directly from this
environment's network egress; this is via web search over secondary
sources -- [Conduktor's Kafka log retention writeup](https://www.conduktor.io/blog/kafka-log-retention-and-segments)
among them -- not Kafka's primary docs verified firsthand, and it is
marked as such rather than presented as a direct citation.) The
active/closed split maps onto this project directly: `Logger::active()`
is already "which segment is the active one," and nothing about closing
one and opening the next needs to touch a live mapping, because closing
a segment here just means *no longer swapping producer threads onto it*
-- the file itself is untouched, exactly like Kafka leaving a closed
segment's bytes alone until retention says otherwise.

**LevelDB and journald** were surveyed in this project's earlier research
(`docs/framing-and-recovery.md`, `docs/prior-art-cpp-loggers.md`) for
framing and queue/allocation behaviour respectively, not for rotation, so
they are not re-cited here as rollover precedent -- reaching for them
now would be padding the prior-art section rather than grounding it.

## What already exists and needs nothing new

Three pieces of the mechanism are not proposals. They were confirmed
against the actual code earlier in this same design conversation, and
restated here because a design doc that re-derives them from scratch
would be less useful than one that points at where they already are:

- **Pre-fetching the next segment ahead of a watermark, without touching
  the current one.** `Logger::create()` builds an independent `Logger`
  that nothing references until something binds it. Nothing about
  building it needs the currently-active `Logger` to pause, because
  nothing shares state between them.
- **Swapping which segment is active costs one atomic exchange.**
  `Logger::active()` is a single `std::atomic<Logger*>`
  (`instance.hpp:401`); `ScopedBind`'s constructor is
  `sActive_.exchange(&logger, ...)` (`instance.hpp:210`).
- **Every producer thread picks up the swap on its own next emit, with no
  coordination beyond that atomic read.** `currentWriter()`
  (`instance.hpp:553`) refreshes its thread-local cache the moment
  `cache.owner_ != this`, i.e. the moment the bound `Logger` has changed
  under it. A thread mid-write into its already-claimed chunk on the old
  segment is completely unaffected -- that mapping is never touched,
  never remapped, never unmapped by any of this.
- **Deleting (or renaming) a segment file while a reader still has it
  open does not fail or corrupt anything.** POSIX `unlink()` on an
  open file is ordinary and safe; on Windows, both the producer's
  `CreateFileW` and the reader's `openReadOnly` already pass
  `FILE_SHARE_DELETE` (`platform.hpp:278,354`), so the same property
  holds there too -- confirmed in this same conversation, not assumed
  from POSIX habit carried over uncritically.

None of this needed inventing. Which is the actual finding of this
section: rollover's *mechanism* is not new work, only its *policy* is.

## The design

Three deliberately decoupled layers. Decoupled because each one was tried
against the invariants separately during the conversation this document
is written up from, and coupling them is where the earlier, rejected
ideas (live compaction, growing a mapping in place) went wrong -- they
tried to make one mechanism solve allocation, retention, and cleanup all
inside a single hot-path operation.

### Layer 1 -- the rotator: watermark-triggered pre-fetch and swap

A caller-side component (not core library code) that polls
`Logger::stats()` the same way an operator already would
(`README.md`, "Watch two counters and one number"), and:

1. When the active segment's usage crosses a configured watermark (a
   fraction of `segmentBytes_`, or a drop-counter-about-to-move signal --
   exact trigger is an open question below, not decided here), calls
   `Logger::create()` for the next segment. This can happen with plenty of
   headroom still in the current one; nothing is wasted by doing it early,
   because the new segment is not used until step 2.
2. Only once the current segment is actually exhausted (or by the
   rotator's own policy, e.g. "close a segment after N minutes even if
   it isn't full" -- a caller decision, not a library one), swaps
   `active()` to the pre-built segment with the one-atomic exchange
   already described above.
3. Marks the old segment retired (see Layer 3) and hands it to whatever
   retention policy is configured.

This alone fixes problem 1 (a segment filling up) without touching
problem 2 (a thread's wasted chunk) -- rollover bounds how long any *one*
segment's waste can accumulate, the same way it already does for total
volume, but it does not reclaim anything mid-segment. That is by design,
not an oversight: nothing that reclaims mid-segment survives contact with
`wire::ChunkHeader::ownerThread_`, per `docs/memory.md`.

### Layer 2 -- the retention policy: how many segments, and what happens to old ones

Purely caller-side orchestration on top of `Logger::create()` and
`Merger` -- no wire format or core library change, because "which
segments exist and for how long" was never something the library needed
an opinion about (`REQUIREMENTS.md`, "Explicitly out of scope").
Configurable shapes, all built the same way:

- **Unbounded (the sensible default).** Every rolled segment is kept.
  Disk usage grows without limit; nothing is ever deleted. This is
  today's manual pattern, just automated by a watermark instead of a
  human remembering to do it.
- **Bounded to N.** Once N segments exist, the oldest is retired for
  deletion as the (N+1)th is created. Framed by the user in this
  design's originating conversation as "the flip-flop double buffer,
  generalised" -- N=2 is exactly that special case, and the general form
  costs nothing extra to support.
- **Bounded by age or by total bytes** instead of segment count -- the
  same mechanism, a different comparison when deciding what's eligible.

The important property, argued through directly in the conversation this
document formalises: **the write side must never block on the retention
side.** A new segment is created and activated on the rotator's own
schedule, regardless of whether cleanup of old segments has kept up. A
slow or stalled compactor (Layer 3) means more retired segments pile up
temporarily, not that logging stops or that a delete call blocks --
exactly the property `FILE_SHARE_DELETE` and POSIX's open-file-unlink
semantics already give for free, described above. N becomes a *target*
the compactor aims to maintain, not a hard cap the allocator enforces by
blocking -- which is also exactly how Kafka's retention check being a
periodic background pass, not a synchronous gate on log rolling, behaves.

### Layer 3 -- the compactor: retirement signal, and what a "reader" does with it

The piece the originating conversation explicitly flagged as open, and
still is here -- this section argues the options rather than picking one,
because unlike Layers 1 and 2, nothing about it was already sitting in
the code waiting to be pointed at.

**Signaling "this segment is retired, safe to compact or delete."** Two
shapes, not equivalent:

- *In-process*, when the rotator and the compactor are the same program
  (e.g. both running as threads in one service): no signal is needed at
  all. The rotator already knows the instant it performs the `active()`
  swap, because it is the one that did it. An in-memory handoff (a queue
  of retired paths) is sufficient and requires nothing new from the wire
  format.
- *Cross-process*, when a separate compactor process needs to discover
  retirement without shared memory: an OS-specific attribute (a Windows
  archive bit, floated in the originating conversation) was considered
  and rejected -- Windows-only, semantically about backup software, not
  fullness, and exactly the kind of platform-asymmetric mechanism this
  project has avoided everywhere else it had a choice (wide paths,
  non-ASCII directories, and share-mode all got portable treatments
  instead of a Windows-only shortcut). The portable alternative: a plain
  filesystem rename on retirement (`foo.s0l` into a `closed/`
  subdirectory, or a suffix), atomic and cheap on every platform Sub0Log
  already treats as first-class, and it fits the existing convention
  exactly -- `sub0log-cat` and this project's own examples already treat
  "every `*.s0l` in a directory" as the unit of work; a `closed/` split
  is the same idiom, not a new one.

**In-process vs. a separate process, as the compactor itself.**
In-process is the better default, not merely an acceptable one.
Compaction touches no producer-path code -- it is read-only decode plus
filesystem operations, the same shape `examples/0_workedexample.cpp`'s
reader thread already is. Nothing about R1.3's no-lock guarantee applies
to a thread that never claims a chunk. Isolating it in a separate process
buys nothing here that an ordinary background thread doesn't already
give for free, unless the deployment specifically wants that isolation
for unrelated reasons (resource limits, a different failure domain).

**A concrete, low-risk optimisation available at this layer, not
previously proposed:** compaction does not have to mean "read the old
segment and delete the file." Because `Segment::create()` already draws a
fresh random generation on every call and the reader already rejects a
generation mismatch (the RocksDB precedent, above), a compactor can
*recycle* a retired segment's on-disk allocation directly -- truncate its
header and hand the same file back to the next `Segment::create()` call,
skipping a fresh `ftruncate` and zero-fill entirely. This needs no wire
format change, because R3.4 was already built to make it safe; it only
needs the file-open path to accept an existing, pre-sized file as a
target instead of always insisting on a fresh one.

**What is genuinely unresolved, and should not be guessed at rather than
tested:** whether an ordinary `std::ifstream` (as `examples/07_live_tail.cpp`,
`tools/sub0log_cat.cpp`, and presumably any compactor built the obvious
way would use) inherits the same delete-while-open tolerance on Windows
that `platform.hpp`'s own `FileMapping::openReadOnly` gets from its
explicit `FILE_SHARE_DELETE`. MSVC's C++ standard library opens a file
under the hood with its own default sharing behaviour, and this was not
checked in the originating conversation -- it was named there as a real
gap, not asserted either way, and it stays that way here rather than
being quietly resolved by assumption. **This is the one item in this
document that needs an experiment, not an argument, before implementation
starts:** does a plain `std::ifstream` opened on Windows block, fail, or
succeed when another process concurrently deletes the file it has open,
and if it doesn't already succeed, does routing the compactor's reads
through `sub0log::detail::FileMapping` instead (which already sets the
right sharing) close the gap for free.

## What this does not change

The wire format: untouched. Every mechanism above operates at file
granularity -- create, swap, rename, delete -- never inside a live
mapping, so none of it touches `wire::cFormatVersion`, R1.3 (no lock on
the producer path), or R3.1/R3.2 (a committed record's durability). This
is the same test the two rejected designs earlier in this conversation
failed: growing a mapping in place risked exactly the base-address
stability every already-claimed `ChunkWriter` depends on; live
compaction risked moving bytes a concurrent reader had already validated.
Segment rollover, built this way, risks neither, because nothing it does
happens to a segment that any thread still has open for writing.

`REQUIREMENTS.md`'s "Explicitly out of scope" stands as written. This
document does not propose the library gain an opinion about retention
policy -- it designs the optional layer a caller assembles from
primitives the library already exposes, the same relationship
`sub0log-cat` already has to `SegmentReader`/`Decoder`.

## Open questions

Recorded rather than resolved, the same way `REQUIREMENTS.md`'s own
"Open questions" section was kept until `architecture.md` actually
answered them:

- **What triggers the pre-fetch watermark precisely** -- a fraction of
  `segmentBytes_` consumed, a rate-based time-to-exhaustion estimate, or
  simply "start the next one as soon as this one exists" (accepting two
  segments' worth of disk pre-allocated at all times as the cost of never
  being caught short)? Each is a different constant-factor trade, none of
  them touches the mechanism above.
- **Does the shipped helper for this live in the library, or purely in
  `examples/`/a separate small header the way `tests/packaging`'s
  CPM-consumer story is documented rather than bundled?** Nothing in this
  design requires it to be part of `Sub0Log::Sub0Log` -- it is caller
  orchestration on public API, the same category as `sub0log-cat` itself.
- **The `std::ifstream`/`FILE_SHARE_DELETE` question above**, which this
  document deliberately leaves as an experiment to run rather than a
  fact to assert.
- **Whether recycling a retired file's disk allocation (the RocksDB-style
  optimisation above) is worth the extra code path over simply deleting
  and letting a fresh `Segment::create()` pay for a new `ftruncate`.**
  The safety argument for it is solid; whether the saved allocation cost
  is worth a second file-reuse code path next to the ordinary
  fresh-create one is a measurement question, not a design one, and
  belongs with the benchmark suite once this is built, not decided here.

## Status

Deferred to v3, same as `architecture.md`'s phasing already said before
this document existed. Recorded now, in this much detail, so that when
v3 actually gets built it is an implementation of Layers 1-3 above --
each already argued against the invariants that matter -- rather than a
rediscovery of the same three false starts (grow the mapping, compact a
live segment, reclaim a chunk mid-life) this project's own conversation
history had to work through once already to arrive here.
