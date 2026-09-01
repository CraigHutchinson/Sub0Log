# The record model, and what it borrows

Most of the shape described here was established by other people. `NanoLog` is
where "the constant half of a call site never enters the record" comes from, and
`Binlog` is where a stream that carries the schema for each event alongside the
events comes from -- "self-describing" is our word for that, not the project's.
Saying so plainly is cheaper than re-deriving either, and it tells a reader
which existing work to go and read.

`REQUIREMENTS.md` R1 and R2 state the contract. This file records the reasoning
behind the shape that satisfies it.

## Split the call site in half

Every log statement has two halves. One is fixed at compile time: the format
string, the subsystem, the severity, the file and line, and the list of argument
types. The other varies per call: the argument values and the timestamp.

A logger that puts both into its queue element has to own the constant half at
runtime, which is where the allocation comes from. Type-erasing a call into a
`std::function<void()>` heap-allocates whenever the capture exceeds the
implementation's small-buffer threshold, which is true of most real call sites,
and any convenience layer that decays a `string_view` argument into an owning
`std::string` allocates a second time without the call site asking for it.

The split puts the constant half in a **site descriptor**: an object with static
storage duration, emitted by the macro that wraps the call, living in the
binary's read-only data. Nothing constructs it, nothing registers it, nothing
allocates for it. The record carries the varying half and a reference to the
descriptor.

## Identity, and why it cannot be only an address

A site's identity in the stream can be the address of its descriptor. That is
free to obtain, stable for the life of the process, and needs no lazy
initialisation -- which matters, because a function-local `static` of
non-trivial type inside a logging accessor is exactly the latched-global shape
that makes a logger untestable.

An address alone is not enough for a decoder that runs later or elsewhere. The
tempting alternative -- let the reader resolve unfamiliar addresses by
dereferencing them -- works for a reader running in the producing process and
fails for the case that matters most. A descriptor belonging to a dynamically
loaded module is only readable while that module is loaded, so an offline decode
of a stream written by a plugin would follow a dangling pointer. This is R4.3,
and it is the reason the definition is written by the producer rather than
resolved by the reader.

So the **first use of a site writes a definition record** naming its format
string, subsystem, severity, argument types and source location. The site
carries a constant-initialised atomic generation counter beside its
descriptor, so there is still no dynamic initialisation and no latched
global, and the emit path compares it against the segment's current
generation with one relaxed load. The steady-state cost is a predictable,
always-false branch; the cold path runs once per call site per segment
generation, not once per process -- a plain once-per-process flag was tried
and was wrong, because it left a site reached again under a Logger rebound
for a new generation (R7.1) writing Messages into a segment that was never
told what the site means.

The property this buys is the strict form of self-description: everything needed
to decode a record precedes it in the stream. A truncated stream therefore
decodes fully up to the point of truncation, rather than becoming undecodable
because the schema was going to be written at the end.

## Where formatting happens

Nowhere in the producing process, in the general case. The format call is made
by whoever decodes: a console reader that tails the live stream for a human
watching a terminal, an offline decoder applying filters, or a test that decodes
into structs and never formats at all.

This inverts the usual arrangement, in which text is primary and any structured
export is produced by formatting. Making the binary record primary is what lets
one record serve a console, a JSON export, a trace-viewer export and a test
assertion, with the formatting cost paid once by whichever consumer wants it --
and in a headless run where nobody wants text, not paid at all.

The console view is a view. If it falls behind, the console is stale. If it
dies, nothing is lost.

## Refusing what cannot be copied

Every argument must be either trivially copyable at the point it enters the
payload or a view over bytes it does not own, enforced by the encoder rather
than by convention: a `std::vector`, a `std::filesystem::path` and a
`std::chrono::duration` do not compile, and the call site must choose between
inlining the bytes and passing an identifier.

That refusal is the point. A silent conversion from a borrowed view to an owning
string is precisely the hidden allocation that profiling finds and that nobody
put in the code deliberately, and the way to stop it recurring is to make it a
compile error rather than a review comment.

**A correction, because this section used to name the wrong culprit.** It said
a `std::string` argument does not compile, and for a while that was true. It
was also wrong: a `std::string` passed by const reference and inlined as bytes
allocates nothing -- it is the `string_view` path with the view taken on the
caller's behalf, and the two compile to the same instructions. The rule this
section is really about is "no allocation and no formatting at the call site",
not "no `std::string`", and conflating them cost every consumer a wrapper at
every string argument for no benefit. `adoption-friction.md` 1.2 has the
measurement; the encoder now accepts anything with a non-throwing conversion to
`std::string_view`, and keeps its full force for the three types above, each of
which needs a representation decision only the call site can make.

## Four mechanisms for variable-length payloads

Chosen by size and by how often the site runs, not by a single rule.

**Inline bytes, for short values.** The bytes are copied into the payload behind
a length, up to the space the record has left. One `memcpy` of a handful of
bytes covers labels, short names, revisions and error codes.

**Continuation records, for the medium case.** A payload that does not fit one
record spills into a bounded number of further records in the same thread's
chunk, chained by a flag and reassembled by the reader. File paths are what this
exists for. Capping the chain puts a ceiling on what one log call can cost the
calling thread, and a payload past the ceiling is truncated with the truncation
recorded in the record's flags, so it is visible rather than silent.

**A stable identifier, for the hot path.** The highest-volume sites usually
describe something the caller already has an identifier for -- a fixed-size
content id already sitting in a register or a cache-resident structure, needing
no string at all. Those sites log the identifier, and the reader resolves it
back to something readable, emitting a definition record the first time it does
so.

The obvious alternative is worse and is rejected explicitly. A producer-side
interner -- hash the string, probe a table, log a small id -- trades a `memcpy`
for a hash, a probe and a first-sight allocation, on exactly the threads whose
largest measured cost is already allocation. Interning is worth having. It
belongs on the consumer's thread, where it is paid once per distinct value
rather than once per event.

**A blob, for the cold bulk case.** Several kilobytes of captured diagnostic
text from elsewhere is not a hot-path payload and should not pretend to be one.
It typically arrives on a thread that has just spent milliseconds waiting, at
most once per occurrence. Allocation is acceptable there and the design should
say so rather than contorting to avoid it: the discipline is "no allocation on a
path that runs per operation", not "no allocation anywhere".

*Not built, deliberately, and this is the reasoning rather than a deferral.*
Held up against the format as it now stands, the case this describes is
already served twice over. A record's payload is bounded by a `u16`, so
"several kilobytes" fits in **one** record with no new mechanism at all --
the two places that stop short of that stop there by policy, not by the
wire: `child.hpp`'s `cLineCap` and the continuation ceiling are both
constants. And where a payload genuinely must span records, continuation
chains are that machinery, built and tested.

What a blob would add beyond those is its *cost contract* -- permission to
allocate. But the producer never allocates to move bytes into the mapping
whatever their size; it `memcpy`s from the caller's memory. The allocation
this paragraph contemplates is the caller's, assembling the text, and that
has already happened by the time any record is written. The one shape that
would genuinely differ is staging the copy off the calling thread, and
`hard-kill.md` rules that out for the property this library exists to keep.

So it would be a second variable-length mechanism, with its own ceiling and
its own tests, delivering what raising a constant delivers. It gets built
when something needs a bulk payload attached to *something other than a call
site* -- where a Message argument is the wrong shape rather than the wrong
size. Until then `RecordKind::Blob` stays a reserved enumerator, because
kind values must be stable whether or not they are ever used.

## The library must not own the vocabulary

A logging library that defines the subsystem enumeration cannot be reused by
anything that needs a different one, and that is the most common way an
extracted logging library ends up not extracted. The library takes an opaque
subsystem id the consumer assigns, and it has no opinion about what a given
id means -- that much does not change below.

What used to stop there was the *spelling*. The consumer kept the id-to-name
table, external to the stream, and a decoder resolved a subsystem by
consulting it -- the one axis where "everything needed to decode a record
precedes it in the stream" was not actually true, because the name was not in
the stream at all. Held up against the strict self-description this file
argues for above, that was a hole: a segment recovered from a machine that no
longer has the producing binary, or even the producing team, decodes every
record to a subsystem id and nothing that says what the id meant.

So a **`SubsystemDefinition` record** does for a subsystem id what a
`SiteDefinition` record does for a call site: it carries the id's name into
the segment itself, written once per id the producer knows about --
declared at `Logger::create()` for the names known up front, or via
`Logger::declareSubsystem()` for one discovered later, such as a plugin
registering itself. A decoder that has never seen a definition for an id (a
segment written before this existed, or one whose producer chose not to
declare a given id) reports an empty name rather than inventing one; the
record it names still decodes.

This does not put the vocabulary back in the library's hands. The library
still does not enumerate subsystems, does not decide which conditions map to
which id, and does not require a single name to be declared -- it only
carries, opaquely, whatever bytes the consumer hands it, the same
undifferentiated byte-carrying `encode.hpp` already does for a site's format
string. What moved is *where the spelling travels*: with the segment now,
rather than in a header that has to be kept forever, by hand, next to
whichever file happens to hold the id assignments.

The severity ladder is the exception and belongs in the library, because the
drop and retention rules key off it -- if severity were consumer-defined, those
rules would be consumer-defined too. What stays with the consumer is the
*policy*: which conditions map to which tier, and what evidence gets attached
when they do. R8.2 is the one constraint the ladder itself has to satisfy, and
it is satisfied by giving the unclassified case a tier above the ordinary error
tier, so that no filter setting can hide it.
