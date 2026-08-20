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
carries a constant-initialised atomic flag beside its descriptor, so there is
still no dynamic initialisation and no latched global, and the emit path checks
it with one relaxed load. The steady-state cost is a predictable, always-false
branch; the cold path runs once per call site per process.

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

Every argument must be trivially copyable at the point it enters the payload,
enforced by the encoder rather than by convention: a `std::string` argument does
not compile, and the call site must choose between inlining the bytes and
passing an identifier.

That refusal is the point. A silent conversion from a borrowed view to an owning
string is precisely the hidden allocation that profiling finds and that nobody
put in the code deliberately, and the way to stop it recurring is to make it a
compile error rather than a review comment.

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

## The library must not own the vocabulary

A logging library that defines the subsystem enumeration cannot be reused by
anything that needs a different one, and that is the most common way an
extracted logging library ends up not extracted. The library takes an opaque
subsystem id plus a consumer-supplied name table, so that a decoder can label
what it reads without the library having an opinion about what the labels are.

The severity ladder is the exception and belongs in the library, because the
drop and retention rules key off it -- if severity were consumer-defined, those
rules would be consumer-defined too. What stays with the consumer is the
*policy*: which conditions map to which tier, and what evidence gets attached
when they do. R8.2 is the one constraint the ladder itself has to satisfy, and
it is satisfied by giving the unclassified case a tier above the ordinary error
tier, so that no filter setting can hide it.
