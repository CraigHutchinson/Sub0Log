# Framing, truncation and recovery

R3.3 asks a reader to tolerate a truncated tail: report how many bytes it could
not interpret rather than failing the whole stream, and never act on a length
field before bounds-checking it. R3.4 adds that recycled storage needs a
generation. Both are cheap to satisfy and expensive to retrofit, and the
existing formats that got each one right are worth naming, because each of them
paid for the lesson.

An early sketch of this design said a reader could "scan forward to the next
plausible header". That is not good enough. The three sections below are the
three mistakes it invited.

Two of the words used throughout -- "self-describing" and "resynchronise" -- are
ours rather than the cited projects'. Where a rationale is our inference rather
than something a project states, it says so. That distinction matters more here
than elsewhere, because these formats are widely paraphrased and the paraphrases
have drifted.

## Mistake one: trusting a length before checking it

**TFRecord checksums the length field separately from the payload.** Its record
layout is a `uint64` length, a masked CRC-32C of those length bytes, the
payload, and a masked CRC-32C of the payload -- the mask being a fixed
rotate-and-add applied so a checksum never appears verbatim in the data it
covers.

The reason the length gets its own checksum is exact: a reader has to know how
many bytes to consume before it can check them, so a torn or corrupt length is
acted upon before any payload checksum could catch it. The result is a wild
read, or a very large bogus allocation, from a file that the payload check would
have rejected a microsecond later. Protecting the payload and leaving the length
unprotected protects the wrong field.

- The canonical implementation now lives in `openxla/xla`, at
  `xla/tsl/lib/io/record_writer.h`; the TensorFlow path is a shim.

**LevelDB makes the opposite choice, and the contrast is the thing to
remember.** Its 7-byte record header is a 4-byte CRC, a 2-byte length and a
1-byte type, all little-endian -- and **the CRC covers the type and the data,
not the length**. Setting the two formats side by side is exactly how this gets
conflated, so it is worth stating in the negative: LevelDB does not protect its
length field, and it does not need to, because block alignment already bounds
what a bad length can do. TFRecord has no such bound, which is why it pays for
the second checksum.

That is the general lesson. Either bound the damage structurally or checksum the
length, but do not assume a payload checksum protects a reader from a length it
already acted on.

The cheap form of the guarantee, for a length-delimited binary stream, is
mandatory and unconditional: **bounds-check the length against the space
remaining in its own container before consuming a single byte**. No length found
in the file can then send a reader outside the block it was found in, whatever
the length says. This costs one comparison and removes an entire class of reader
crash on damaged input.

**Commit last.** A record is committed by writing a commit flag in its header
*after* the payload, behind a release fence, so a reader trusts only records
whose commit flag is set and a torn record is simply where decoding of that
block stops. This is cheaper than a per-record CRC and is the right tool for
this particular threat: inside a mapped file, the only corruption source is a
write that did not finish. A verification mode that adds a CRC over header and
payload addresses the different threat of silent corruption, and that one is
opt-in because it is not free.

**Order the header so validation touches one word.** Everything a reader needs
in order to decide whether a record is intact and how far to advance -- the sync
pattern, the length, the sequence number, the flags -- belongs at the front of
the header, so validation completes before any pointer moves.

## Mistake two: framing that lets a reader stop but not resume

**RFC 7464** (JSON Text Sequences) defines the stream as
`JSON-sequence = *(RS JSON-text LF)`: a record separator, `0x1E`, *before* each
JSON text, and a line feed after it.

Two details are worth having right, because the format is usually paraphrased
loosely. The trailing `LF` is an **encoder obligation only** -- the parser
grammar tolerates its absence -- so a reader cannot delimit on it, and its
documented purpose is narrower than framing: it is what lets a truncated
top-level number be detected as truncated. The leading `RS` is the part a reader
uses, and the RFC's own phrasing for recovery is to *skip to the next `RS`*. It
never uses the word "resynchronise"; that is our word for what skipping
achieves.

The contrast with bare newline-delimited JSON is the payoff. A torn final line
there leaves a fragment that a naive consumer may parse as a valid prefix, and
nothing marks it as incomplete.

- https://www.rfc-editor.org/rfc/rfc7464

The consequence for a binary format is that the record header wants a
**distinguished multi-byte sync pattern** rather than a single magic bit, so
finding the next record is a search for something improbable rather than for
something merely plausible.

**Avro's object container files** take the strongest version of that idea. Each
file carries a 16-byte sync marker, **generated at random for that file**,
written in the header and again after every data block. The randomness is what
makes it work: a fixed magic number can occur inside payload data by accident,
so a reader scanning for it can land on a false boundary, whereas a per-file
random marker makes that outcome negligible.

Note what the specification actually claims for it, because it is less than the
paraphrase. Avro states two purposes -- splitting a file for parallel
MapReduce-style processing, and detecting corrupt blocks. It does not describe
the marker as a resynchronisation mechanism; that is our reading of a field that
happens to support it.

- https://avro.apache.org/docs/current/specification/

**LevelDB's log format** shows what block alignment buys on top of framing. The
file is a sequence of 32 KiB blocks. A record too large for the space remaining
is fragmented across blocks using first, middle and last types alongside the
full type, and a record never starts within the last six bytes of a block --
those bytes are padding, and the case of exactly seven bytes remaining is
handled by writing an empty first-fragment record rather than by special-casing
the reader.

A reader that hits damage skips to the next block boundary and resumes. The cost
of damage is therefore bounded to one block, and the reader never has to scan
byte by byte looking for a boundary, because it knows where boundaries are
allowed to be. That is a good argument for a chunked layout even in a format
whose records are individually length-delimited.

- `doc/log_format.md` in the LevelDB repository

## Ordering: the version field goes before the checksum

**Kafka's record batch header places the `magic` byte -- the format version --
before the `crc`.** The ordering really did change: v0 and v1 put the checksum
first, and v2 moved it after the version byte.

What the documentation states is that a client must parse the magic byte before
deciding how to interpret the bytes between the batch length and the magic byte,
and the constraint KIP-98 records is that fields *before* the magic byte cannot
change between versions. The tidy explanation -- "so a reader can determine the
version before validating a checksum whose definition may have changed" -- is
**our inference** from those two statements rather than something any primary
source says. It is a short inference and it is consistent with the observed
change, but it should be labelled, not quoted.

The design consequence stands either way: anything whose interpretation a
version bump could change belongs behind the version field, not in front of it.
Put the checksum first and today's checksum definition is in the format
permanently.

Kafka's CRC-32C covers the attributes field to the end of the batch, and
deliberately excludes `partitionLeaderEpoch` so that a broker can update that
field without recomputing the checksum -- a second, separate lesson about what a
checksum should and should not cover.

- Kafka protocol documentation, "Record Batch" section:
  https://kafka.apache.org/documentation/#recordbatch

## Mistake three: recycling storage without a generation

**RocksDB's recycled log files are the precedent.** Reusing a pre-sized file
(`recycle_log_file_num`, in `DBOptions`) avoids the cost of creating and zeroing
a new one, which is a real saving. The trap is that the bytes left behind from
the file's previous life are structurally valid: plausible length, decodable
record.

RocksDB's answer is a second header layout. The ordinary record header is 7
bytes; the recyclable one is 11, the extra four carrying the log number, and
`log_reader.cc` compares that number against the log it expects and returns an
old-record result when they differ.

The stated rationale is modest -- to distinguish records written by the most
recent log writer from those written by a previous one. The stronger version
often given, that the stale bytes would otherwise pass their checksum and be
replayed as live data, is **inference**: it follows from the mechanism and it is
almost certainly why the mechanism exists, but it is not documented anywhere.
Marked as inference because it is the sentence a reader is most likely to repeat
as fact.

Any design that pre-sizes its storage invites exactly that optimisation, so the
defence belongs in the format from the start rather than being added after
somebody adds recycling. **Stamp the generation on the block rather than on the
record** and check it against the stream header before decoding anything in that
block. One field per block is free, and it is sound as long as a block is filled
by one writer within one generation.

This also removes any reliance on zero-fill as the end-of-data signal, which is
the weakest part of a naive design: a generation mismatch is positive evidence,
where a zero byte is merely an absence. R3.4 states the requirement in the form
this reasoning produced -- absence of data is not a reliable end-of-stream
marker.

## The recovery-policy taxonomy

RocksDB's `WALRecoveryMode`, in `include/rocksdb/options.h`, is the clearest
published enumeration of the choices, and a diagnostic stream should pick from
it deliberately rather than by accident. Its own default is point-in-time
recovery.

**Tolerate a corrupted tail** (`kTolerateCorruptedTailRecords`). Damage at the
end of the file is expected -- that is what an interrupted writer leaves -- so
decode the prefix and report the tail as lost. This is the right default for a
stream whose writer may be killed at any instant, and it is what R3.3 asks for.

**Absolute consistency** (`kAbsoluteConsistency`). Any damage anywhere fails the
whole file. Right for a database that must not silently lose a committed
transaction; wrong for a diagnostic stream, where refusing to show the
ninety-nine per cent that decoded because of one torn record at the end helps
nobody.

**Point-in-time recovery** (`kPointInTimeRecovery`). Stop at the first damage
and keep everything before it, treating the remainder as absent. The honest
middle position when damage can occur anywhere rather than only at the tail.

**Skip any corrupted record** (`kSkipAnyCorruptedRecords`). Continue past damage
and keep going. Only safe when the framing supports genuine resynchronisation --
an improbable sync pattern, or block alignment -- and only acceptable when the
reader reports how much it skipped. Silently skipping is how a reader turns
damage into a wrong answer.

Whichever policy is chosen, the count of bytes that could not be interpreted is
part of the reader's output rather than a line it prints on the side. A recovery
that does not report what it lost is indistinguishable, to its caller, from one
that lost nothing.

## What this adds up to

Because each definition is written before its first use (`record-model.md`), a
truncated stream decodes fully up to its truncation point. A torn record is
reported as torn rather than skipped. A stale block from a previous generation
is rejected on positive evidence rather than assumed away. And the reader hands
its caller a number for what it could not read, so absence is never mistaken for
silence.
