# Requirements

What this library must do, and the constraints that shape it. Written before the
implementation so it can be argued with cheaply.

Each requirement says what it rules out, because a requirement that forbids
nothing is decoration.

## 1. The producer does no work it can defer

**R1.1** A call site must not format. No `std::format`, no `to_string`, no
stream insertion on the calling thread. Formatting happens in whatever process
or thread later decodes the stream.

**R1.2** A call site must not allocate. No heap traffic on any path a producer
takes, including when a variable-length argument is large. The strategy for
oversized payloads is truncation or a separate blob channel -- never a
`malloc` on the calling thread.

**R1.3** A call site must not take a lock. Chunk acquisition is a single atomic
read-modify-write; writing into an acquired chunk is unsynchronised because the
chunk belongs to one thread.

**R1.4** A disabled call site costs a relaxed atomic load and a comparison, and
does not evaluate its arguments. This is what allows verbose diagnostics to be
compiled into release builds rather than hidden behind rebuild-only flags.

*Rules out:* a formatting frontend, an unbounded queue that allocates when it
grows, and any design where enabling a subsystem requires a rebuild.

## 2. Records reach the consumer typed

**R2.1** A consumer receives argument values in their original types. An
integer arrives as an integer, not as characters that must be parsed back.

**R2.2** Records are filterable after the fact by subsystem, severity,
timestamp, thread and correlation id, without parsing message text.

**R2.3** A test can assert on a field. Asserting on a substring of a rendered
message is a fallback, not the mechanism.

*Rules out:* any design where the sink boundary is a formatted string, which is
where most existing loggers place it.

## 3. The stream survives an abrupt exit

**R3.1** A record that has been committed must survive `TerminateProcess` /
`SIGKILL` of the producing process. Records are written into memory the kernel
owns, not into a staging buffer the process owns.

**R3.2** No graceful shutdown, signal handler or flush-on-exit may be required
for R3.1 to hold. Those paths do not run on a hard kill.

**R3.3** A reader must tolerate a truncated tail: it reports how many bytes it
could not interpret rather than failing the whole stream, and never acts on a
length field before bounds-checking it.

**R3.4** If storage is pre-sized or recycled, every record must carry a
generation so that stale-but-structurally-valid data from a previous run cannot
be read as current. Absence of data is not a reliable end-of-stream marker.

*Rules out:* a background writer thread as the only path to durability, and any
framing whose recovery depends on scanning for a plausible-looking header.

## 4. Safe to link into a plugin

**R4.1** A dynamically-loaded plugin must be able to emit records into its
host's stream without linking the library's implementation, and without the
duplicated-singleton failure that static-linking a logger into two modules
produces.

**R4.2** The plugin-facing boundary is C ABI and dependency-free: no JSON
parser, no allocator, no C++ runtime state crossing the boundary.

**R4.3** A record emitted by a plugin must remain decodable after that plugin
has been unloaded, and after the process has exited. Descriptors are written by
the producer, not resolved by the reader against a live module.

*Rules out:* requiring plugins to link the full library, and any scheme where
decoding needs the producing binary still to be loaded.

## 5. Several processes, one stream

A single-process logger is a solved problem. The interesting cases are a service
plus its command-line tools, a supervisor plus its workers, and any process that
shells out to a third-party executable -- and in all of them the question a
reader has is what happened *across* the processes, in order.

**R5.1** Each process writes its own segment. There is no shared writable
structure between processes, no cross-process lock, and no collector daemon that
must be running for logging to work. A process that dies cannot corrupt or block
another's stream.

**R5.2** A decoder merges segments into one ordered stream. Ordering across
processes is the decoder's job, not the producer's -- producers never coordinate.

**R5.3** Timestamps must be comparable across processes on one machine. This is
the requirement most easily assumed and most easily wrong: the C++ standard
leaves `steady_clock`'s epoch unspecified, so two processes' readings are not
portably comparable even on the same host. Records therefore carry a reading
from a machine-wide monotonic source, and each segment carries an anchor record
pairing that source with a wall-clock reading, so a merger can align segments
whose epochs differ without depending on an implementation detail that happens
to work today.

**R5.4** A correlation identifier propagates into child processes, so work a
child performs joins the activity that spawned it. The mechanism must survive
`exec` of an unmodified binary, which in practice means the environment rather
than an inherited handle.

**R5.5** A child that cannot be asked to cooperate -- any third-party executable
-- is covered by capturing its output streams and emitting them as records
attributed to that child, carrying its command line, exit status and the
correlation id in scope when it was spawned. The library must not require a
child to link anything.

*Rules out:* a shared ring buffer with a cross-process claim protocol, a
collector process on the critical path, ordering by arrival at a merger, and any
design where a spawned tool's diagnostics are simply lost.

## 6. Correlation is a field

**R6.1** An identifier set by an RAII scope is attached automatically to every
record emitted within it, across threads and across the plugin boundary.

**R6.2** Joining related records is an equality test on that field, not a
heuristic over timestamps or message text.

## 7. Deterministic in tests

**R7.1** A test can bind its own instance for the duration of a scope, following
the established scoped-active pattern, and drain it synchronously.

**R7.2** No test-only branch in production code paths. If a seam is needed, it
is present in every build.

*Rules out:* `#if TEST_BUILD` bypasses, and any global that cannot be overridden
for the duration of a test.

## 8. Portability

**R8.1** Windows/MSVC, Linux and macOS with Clang and GCC, all first-class.
Platform-specific code confined to a mapping layer with one interface.

**R8.2** C++26 target, C++23 floor: every header compiles as C++23 until all
first-class toolchains ship C++26, and C++26-only features are gated on their
feature-test macros. No compiler extensions on any path where a standard feature
exists; where an extension is genuinely required, it is isolated and documented
with what was tried instead.

**R8.3** No third-party runtime dependency in the producer path.

## 9. Observability of the mechanism itself

**R9.1** Dropped records are counted and the count is retrievable. A drop is
never silent.

**R9.2** A failure the library cannot classify must be more visible than one it
can, never less. Unrecognised conditions carry more evidence forward, not less.

## Explicitly out of scope

Log rotation policy, network transport, a query language, and any dependency on
a running collector daemon. Aggregation across *machines* is out too: it needs a
transport and a clock-synchronisation story, both of which are a different
problem from the one-host case in section 5 and would drag a dependency into a
library that currently has none. The library produces a stream and a decoder; what
consumes them is the caller's business.

## Open questions

- Whether the descriptor is addressed by pointer or by a content-derived id,
  and what that costs when a plugin is unloaded mid-stream.
- Chunk size, and whether it should differ between a high-rate producer and an
  occasional one.
- Whether the decoder ships as a library, a CLI, or both.
