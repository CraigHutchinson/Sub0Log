# vNext: chunk-header checksum

A sibling to `vnext-adaptive-chunk-sizing.md`, not a layer of it, and
narrower in scope than either vNext document before it: it adds exactly
one field, `wire::ChunkHeader::headerChecksum_`, and answers exactly one
question a reader could not previously ask -- *do this chunk's own header
bytes still say what they said when they were stamped?* -- as opposed to
*how far do I stride past them*, which `chunkSizeClass_` already answers.
Implemented and merged; there is no further phase.

## The gap this closes

`docs/architecture.md` already states the position this document assumes
rather than re-argues: "There is no per-record CRC on the default path:
inside a mapped file the threat is an unfinished write, and commit-last
answers exactly that threat," and separately, "A verification mode that
adds a CRC over header and payload addresses the different threat of
silent corruption, and that one is opt-in because it is not free." That
per-record, opt-in, header-and-payload mode is not what this document
builds. What it builds is narrower and unconditional: a per-*chunk*
checksum over the *header* alone, always present once a producer stamps
one, costing one CRC-32 computation per `Segment::claimChunk()` call --
not per record, and not gated behind an opt-in flag, because the fields it
protects are exactly the ones `SegmentReader::visit()` already decodes
unconditionally on every chunk it visits (`generation_`, `ownerThread_`,
`chunkSizeClass_`) plus one it decodes for its own sake (`claimMonoNs_`).

Before this field existed, a chunk header's four meaningful values were
trusted verbatim once two structural checks passed: `generation_` equal to
the segment's own, and (as of `vnext-adaptive-chunk-sizing.md`)
`chunkSizeClass_` no greater than `wire::cMaxChunkSizeClass`. Both checks
are checks on *plausibility*, not on *integrity* -- a bit flip that turns a
legitimate `chunkSizeClass_` of 3 into an equally legitimate-looking 5, or
that turns `ownerThread_` into some other thread's id, passes both checks
without comment. R2.2's thread filtering would then silently attribute a
record to the wrong thread, and Phase 1.5's self-describing navigation
would silently stride by the wrong amount -- not a crash, and not a torn
record R3.3 already knows how to report, but a wrong answer delivered with
full confidence.

## The encoding

`wire::ChunkHeader` gained one 4-byte field, `headerChecksum_`
(`wire.hpp`), placed immediately after `claimMonoNs_` and before
`chunkSizeClass_` -- not appended at the end, which is the placement every
earlier additive field in this format used. The reason is alignment, not
habit: at byte offset 24 (immediately after three `uint64_t` fields) a
`uint32_t` lands 4-byte aligned with no compiler-inserted padding, keeping
`sizeof(ChunkHeader)` at exactly 32 with zero implicit bytes either
field would otherwise cost. This was confirmed against a real compiler
before the struct was changed, not assumed from the arithmetic alone: a
standalone compile probe with `offsetof` on the proposed layout
(`generation_`, `ownerThread_`, `claimMonoNs_`, `headerChecksum_`,
`chunkSizeClass_`, `reserved_[3]`) against GCC 13 with `-Wpadded` showed
the four leading fields at offsets 0/8/16/24, `chunkSizeClass_` at 28, and
no warning -- the same discipline `vnext-adaptive-chunk-sizing.md` used to
confirm its own `chunkSizeClass_`/`reserved_[7]` split before relying on
it. `reserved_` shrinks from seven bytes to three; the struct's total size
and its `is_trivially_copyable`/`sizeof == 32` static asserts are
unchanged.

**Why 0 is the sentinel, and why that needs no format-version bump.** The
same reasoning `chunkSizeClass_` already established, one field over.
`wire::cFormatVersion`'s own comment states the standing rule: bump it
only when an old reader would otherwise be misled, never as a reflex for
"the struct changed." Every chunk stamped before this field existed left
these four bytes as silent, always-zero reserved space, which is exactly
what "not present" means; a never-claimed chunk (`generation_ == 0`) has
the same all-zero header regardless of when it was written. An old reader
still never looks at the field (unchanged). A new reader meeting either
case sees 0 and skips verification, which is exactly the legacy behaviour
-- trusting the plausibility checks alone, nothing more, nothing less.
Neither direction needs a version gate.

**Why a genuine computation is never allowed to land on 0.**
`computeChunkHeaderChecksum()` (`wire.hpp`) remaps a raw CRC-32 result of
exactly 0 to 1 before returning it. Standard CRC-32 (init `0xFFFFFFFF`,
final XOR `0xFFFFFFFF` -- the ISO-HDLC/zlib variant, verified against the
well-known `crc32("123456789") == 0xCBF43926` check value in
`tests/unit/wire.test.cpp`) essentially never produces 0 for real input in
practice, but "essentially never" is not the same claim as "never," and
this field's entire backward-compatibility argument rests on 0 meaning
one specific thing unambiguously. Rather than accept a one-in-four-billion
chance of a real, correctly-stamped chunk being misread as "not present"
(which would only *weaken* verification for that one chunk, never corrupt
it -- the same residual class of harm a stored 0 already tolerates for
every legacy chunk), the remap closes the gap outright at the cost of one
comparison per stamp. The same technique appears elsewhere for the same
reason: a UDP checksum that computes to 0 is transmitted as all-ones,
because 0 in that header already means "no checksum."

**What is checksummed, and why it is not one contiguous memcpy.** Four
fields: `generation_`, `ownerThread_`, `claimMonoNs_`, `chunkSizeClass_`
-- the ones a reader actually decodes and acts on, not the three
still-reserved padding bytes past `chunkSizeClass_`, which carry no
meaning to protect. Because `headerChecksum_` itself sits between
`claimMonoNs_` and `chunkSizeClass_` in the on-disk layout (the alignment
placement above), those four fields are not one contiguous byte range of
the stamped struct -- `computeChunkHeaderChecksum()` runs `wire::crc32()`
twice, chained the way zlib's own `crc32()` is (the value returned from
one call feeds the next as the running `crc`, starting from 0), first
over the three leading `uint64_t` fields as 24 contiguous bytes, then over
`chunkSizeClass_` as a single further byte. `wire::crc32()` itself is
bit-by-bit rather than table-driven: it runs at most once per
`Segment::claimChunk()`, not per record, so a 256-entry lookup table would
trade static data for a saving nothing on this path needs.

## Where it is checked, and why the order matters

`Segment::claimChunk()` (`segment.hpp`) computes and stamps
`headerChecksum_` last, after every other field it assigns, from a single
call to `computeChunkHeaderChecksum()` -- one function owns the split
between "fields covered" and "the checksum field itself," rather than
repeating four inline `crc32()` calls at the one production call site.
That call site was also changed from positional aggregate-initialisation
braces (`wire::ChunkHeader{generation_, currentThreadId(), monotonicNowNs(),
sizeClass_}`) to field-by-field assignment: the positional form is exactly
what would have silently fed `sizeClass_` into the new `headerChecksum_`
slot instead of `chunkSizeClass_` had the struct's field order changed
under it without the call site changing too -- which is precisely what
just happened once, here, to `chunkSizeClass_` itself relative to
`reserved_`. Named assignment survives the next field the struct gains the
same way this one did; positional braces do not. The same fix was applied
to `tests/support/segment_image.hpp`'s two existing `ChunkHeader`-building
helpers, which had the identical latent bug for the identical reason --
found by grepping for every `wire::ChunkHeader{` construction in the tree
before relying on any of them, not assumed safe because the file compiled.

`SegmentReader::visit()` (`reader.hpp`) verifies the checksum immediately
after loading a chunk's header and *before* the existing
`chunkHead.generation_ == 0u` branch -- not folded into the
generation-mismatch branch further down, and not placed after either
branch as an additional cross-check. This ordering is the actual design
decision in this document; everything above it is mechanism. The
`generation_ == 0u` branch exists to mean "never claimed" and is trusted
today on the strength of one fact: a fresh segment is `ftruncate`d to its
full size, so every byte of an unclaimed chunk, this field included, is
genuinely zero. But corruption is not obliged to leave `generation_`
alone -- a bit flip could just as easily zero the `generation_` field of a
real, previously-claimed, non-zero-generation chunk as it could touch any
other field, and a reader that checked the checksum only *after* branching
on `generation_ == 0u` would already have misread that corrupted-but-real
chunk as "never claimed" -- unwritten space, not damage -- before the
checksum ever got a chance to say otherwise. Checking the checksum first
is what keeps those two cases from being conflated: a checksum mismatch is
now conclusive evidence that nothing in this header, `generation_`
included, is trustworthy, decided before any of it is acted on.

A checksum mismatch is treated exactly like every other header-level fault
this function already handles below it -- the stale-generation branch, the
out-of-range-size-class branch -- rather than as a new category: count the
chunk's body as unreadable, stride past it using the segment's own uniform
`header_.chunkBytes_` (nothing about a header that failed its own
integrity check is trustworthy enough to steer navigation by, including
whatever size it might itself claim), and continue to the next chunk.

## Proof, not assertion

`tests/unit/wire.test.cpp`: `crc32()` against the standard CRC-32/ISO-HDLC
check value for `"123456789"`; chaining (one call over N bytes equals two
calls over any split of the same bytes); `computeChunkHeaderChecksum()`
changing when any one of its four inputs changes, independently; and the
zero-remap holding across a spread of inputs including the specific
all-zero case (`0, 0, 0, 0`) where a genuine checksum and the reserved
sentinel could otherwise collide.

`tests/integration/reader.test.cpp`: a chunk whose stamped checksum
matches decodes with no extra damage; a checksum of 0 is trusted exactly
as a segment built before this field existed always was (the same
backward-compatibility claim `chunkSizeClass_` makes, tested the same
way); a header whose checksummed fields were tampered with *after* being
correctly stamped is reported as damage, not decoded; and -- the test this
document exists to make pass -- a chunk stamped with a real, matching
checksum whose `generation_` bytes are then directly zeroed is still
reported as damage rather than as unwritten space, proving the ordering
above is load-bearing and not merely tidy. 111/111 tests in the suite (102
before this change, 0 rewritten, 9 added: 5 unit, 4 integration), clean
under a plain build and under ASan+UBSan both.

## What this does not do

This is not the opt-in, per-record, header-and-payload CRC mode
`docs/architecture.md` already names as a different, deliberately deferred
answer to a different threat -- silent corruption of a record's own
payload bytes, which commit-last says nothing about because commit-last's
job is only ever "did the write finish," not "did the bytes stay correct
afterward." That mode remains unbuilt and this document does not narrow
the argument for it: a header this small (32 bytes, four meaningful
fields) is cheap enough to checksum unconditionally, on every chunk claim,
in a way a payload of arbitrary and sometimes large size is not, so
neither its cost argument nor its opt-in framing transfers. Nor is this a
mechanism for reconstructing corrupted data -- a checksum tells a reader
that a chunk's header is not to be trusted, the same "stop, count, move
on" response every other header-level fault already gets; it does not
recover the bytes that were there before corruption, and no format this
small carries the redundancy that would let it.

## What this does not change

R1.3 (no lock on the producer path) is untouched: `claimChunk()` computes
one CRC-32 over 25 bytes as part of the same single, already-uncontended
stamp it was performing before, still with exactly one `fetch_add` as the
only cross-thread synchronisation on the path. R3.1/R3.2 are untouched --
nothing here changes when or how a record becomes durable, only whether a
reader trusts the header that tells it where to look. `cFormatVersion`
stays at 1, for the same reason `chunkSizeClass_` needed no bump: this is
additive, and an old reader's behaviour on both old and new data is
unchanged. `Segment::create()` still produces byte-for-byte the same
segment for the same `SegmentOptions`, aside from the four checksum bytes
now living inside each chunk header that used to be silent zero.

## Status

Implemented, tested (111/111 including ASan+UBSan), no further phase
planned. The opt-in per-record header-and-payload CRC mode
`docs/architecture.md` already names remains a separate, unbuilt design
with its own cost/benefit argument this document does not make on its
behalf.
