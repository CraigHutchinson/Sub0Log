# Prior art for the vNext design: backends, channels and allocators

`vnext-backends-and-memory.md` proposes a selectable storage backend,
per-call-site arenas, and injectable allocators off the hot path. Before any
of that gets built, the honest question: is any of it new, or is it already
solved somewhere and we are about to reinvent it badly?

Mostly the latter, and that is the good outcome. Three separate traditions
have each built a piece of this, each has a name for it, and each warns
about a different mistake. The result changes two decisions and confirms
the rest.

## 1. "Front-end / backend" is taken, and it names the thing we reject

Boost.Log splits every sink into a **frontend** and a **backend**. The
frontend owns "thread synchronization model, filtering and, for text-based
sinks, formatting"; the backend "defines the processing rules of the log
records" -- writing to a file, for instance. Its `asynchronous_sink`
frontend passes records to the backend "in a dedicated thread", which is
what makes a slow backend tolerable.

That is a coherent design and it is *not* ours. Boost.Log's seam sits at a
record queue with a worker thread behind it -- precisely the staging
arrangement `hard-kill.md` rules out, because anything sitting in that queue
dies with the process. Our seam sits at the chunk source, below the record,
where no queue exists.

**Decision: stop calling it a backend.** A reader who knows Boost.Log will
read "backend" and correctly infer a queue and a thread, and be wrong about
this library in the one way that matters most. The policy should be named
for what it actually varies -- `ChunkSource`, or storage policy -- and the
public API should never say "frontend/backend". `vnext-frontend-backend.md`
keeps the term in its title because that is the discussion it records; the
implementation should not inherit it.

## 2. The compile-time options policy is quill's, and it already works

Quill parameterises its frontend with a compile-time options struct passed
as a template argument -- `queue_type`, `initial_queue_capacity`,
`blocking_queue_retry_interval_ns`, `huge_pages_enabled` -- as static
`constexpr` members of a user-defined struct. That is exactly the shape
proposed here: one template parameter, a default that nobody has to name,
and no cost unless you use it.

Worth copying: **one options struct, not N template parameters.** Worth
heeding as a warning: quill issue #609 reports that mixing a component built
with custom `FrontendOptions` and one built with the defaults produces a
compile error in one direction and a runtime assertion in the other. That is
the type-identity cliff the standard's own allocator papers describe, and it
is the single most likely way this feature makes the library harder to use.
Any policy we add must keep the common path monomorphic and must not appear
in signatures an ordinary consumer reads.

## 3. Per-region buffers are LTTng channels, and the argument is already made there

LTTng's **channels** carry per-channel `--subbuf-size` and `--num-subbuf`,
and its buffering schemes let a domain use per-process buffers rather than
one global ring. The documented rationale for per-process buffering is that
"one process having a high event throughput won't fill all the shared
sub-buffers, only its own, though it consumes more memory".

That is the blast-radius argument for per-call-site arenas, made by a system
that shipped it, along with the cost -- more memory -- stated in the same
breath. Two consequences:

- **Use the word channel.** It already means "a named group of events with
  its own buffer configuration", which is what a hot region wants. "Arena"
  suggests an allocator, which is the confusion this design exists to avoid.
- **Copy the configuration axes** (buffer size × count) rather than
  inventing new ones, and state the memory cost where LTTng states it.

Where we differ, and it is the interesting part: LTTng needs a session
daemon and a configured session before anything is recorded, and its
channels are configured out-of-band. Ours would be a compile-time constant
beside the call site, and a channel's records merge back into one ordered
stream through the `Merger` that already exists, because a channel is just
another segment. No daemon, no configuration step, nothing to have forgotten
to switch on.

## 4. The allocator question was litigated in spdlog, and the outcome is instructive

spdlog PR #691 proposed exactly this feature: overridable `allocate` /
`deallocate`, defaulting to `std::malloc`/`std::free`, motivated by console
development (Xbox One, PS4) where memory budgets are strict. The maintainer
questioned the need, arguing spdlog allocates mainly at initialisation
rather than on the hot path. The author answered that strict budgeting
requires controlling *all* allocations regardless of frequency. It was
closed unmerged in 2019.

Both sides were right about different things, and that is the lesson:

- The maintainer's argument is the same one made in
  `vnext-backends-and-memory.md` -- the hot path barely allocates, so an
  allocator hook there has little to do. That remains true and it is **not
  a refutation of the requirement**. Budget accountability is a different
  requirement from hot-path latency: a platform that demands every byte be
  attributable does not care that the allocations are infrequent.
- The proposed *mechanism* was the wrong shape. Global mutable function
  pointers are a process-wide latched global -- action at a distance, no
  scoping, untestable, and exactly the pattern R7 rejects everywhere else
  in this library. The scoped alternatives (a `memory_resource` handed to
  the object that will use it, a policy on a type) do the same job without
  it.

So: build it, for the reason the requester gave rather than the reason a
performance argument would suggest, and do not build it the way that PR did.

## 5. The standard already split the two mechanisms, for our reasons

The polymorphic-allocator papers (N3916, P0339) set out the trade directly:
an `Allocator` template parameter changes the type -- `basic_string` with
two different allocators are two different types, which "inhibits the use of
vocabulary types" -- while `pmr::memory_resource` keeps one type and pays a
virtual call per allocation.

That is precisely the split proposed here, applied per path: a template
policy for producer-side setup, where indirection is not affordable and the
type is not a vocabulary type; `pmr` for the decoder and merger, where a
virtual call per allocation is free at tens of nanoseconds per record and
keeping `Decoder` a single type matters more.

## What is actually ours

Very little, which is the right answer for a design this close to solved
problems. The combination is the contribution:

1. A storage seam at **chunk** granularity rather than at a record queue --
   forced by a durability requirement most loggers do not make, and the
   reason Boost.Log's vocabulary does not fit.
2. Channels whose records **merge back into one ordered stream at read**,
   with no daemon and no session, because the multi-process design already
   requires exactly that machinery.
3. The rule that **an allocator hook may never be reachable from a call
   site's emit path** -- which is what lets the feature exist without
   quietly costing the property the library is for.

Everything else should use the vocabulary that already exists.

## The constraint none of this may break

The default must stay as simple as any ordinary logger, and customisation
must be opt-in and free when unused. Concretely, checked against the prior
art above:

- A default template argument keeps `Logger` exactly what it is today; a
  consumer who never wants a channel or an allocator never types one.
- Policy types must not surface in the signatures a normal consumer reads
  (quill #609 is the cautionary tale: a policy that escapes into public
  types turns a convenience into an interoperability problem).
- `pmr` on the reader keeps `Decoder` a single type, so a bounded decode is
  available without changing anybody else's declarations.
- No global mutable hooks (spdlog #691's shape) anywhere.
- Nothing above changes the wire format, so a channel, a memory segment and
  a file all read back through the same decoder.

## Sources

- Boost.Log, sink frontends and backends:
  https://boost-log.sourceforge.net/libs/log/doc/html/log/detailed/sink_frontends.html
  and `.../sink_backends.html`
- Quill frontend options:
  https://quillcpp.readthedocs.io/en/latest/frontend_options.html ; the
  mixing cliff: https://github.com/odygrd/quill/issues/609
- LTTng channels and buffering schemes:
  https://lttng.org/man/1/lttng-enable-channel/v2.13/
- spdlog allocator support, proposed and closed:
  https://github.com/gabime/spdlog/pull/691
- Polymorphic memory resources, rationale: N3916
  (https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n3916.pdf) and
  P0339 (https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p0339r6.pdf)
