# vNext: the backend ladder, and who decides where memory comes from

Two directions that look like one and are not:

1. a **selectable backend** so a consumer can start on a naive
   single-process store and scale up to the hard-kill-durable,
   multi-process one without changing a call site; and
2. **customisable memory management** -- invisible by default, reachable by
   template, and potentially specialised *per call site* for deep,
   performance-critical regions.

**Two naming decisions come from `prior-art-backends-and-memory.md`, and the
rest of this file uses them.** "Backend" is Boost.Log's word for a sink
behind a record queue and a worker thread -- the arrangement `hard-kill.md`
rejects -- so the policy is a `ChunkSource`, or storage policy, and the
public API never says frontend/backend. And a per-region buffer is a
**channel**, LTTng's word for a named group of events with its own buffer
configuration, rather than an "arena", which suggests the allocator this
design exists to keep away from the emit path.

`vnext-frontend-backend.md` established where the storage seam may go and
why the obvious queue-shaped split is off the table. This file takes the
next step: what the ladder's rungs actually are, and -- the part that needs
care -- what "inject an allocator" can and cannot mean in a library whose
hot path does not allocate at all.

## Part one: the ladder

One policy parameter, `ChunkSource`, chosen at compile time.
`Logger` stays an alias for the full one, so nothing a consumer writes
today changes.

| rung | backend | keeps | gives up | for |
|---|---|---|---|---|
| 1 | `MemorySegment` | format, decoder, typed records | R3 durability, R5 multi-process | embedded, tests, tools, "just show me records" |
| 2 | `FileSegment` (today) | everything | -- | services, anything post-mortem matters for |
| 3 | `FileSegment` + rotation / flush-on-fatal | everything, bounded on disk | a syscall where you ask for one | long-running services |

Rung 1 is the naive one and is worth building first, because it is what
makes the library usable where there is no filesystem, and because it
proves the seam is real. It writes the identical wire format into a buffer
the consumer owns; the same `SegmentReader`, `Decoder` and `Merger` read it
with no knowledge that it was never a file.

What must not vary across rungs: the wire format, commit-last, the
descriptor split, the encode rules. A backend chooses *where bytes live and
how durable they are*, never what they mean. One decoder reads all of them,
and a `MemorySegment` dump merges with a `FileSegment` file.

## Part two: memory, and the reframe that matters

The instinct is to add an `Allocator` template parameter beside
`ChunkSource` and let it flow everywhere. That would be a mistake, because
it answers a question the hot path does not ask.

**The emit path allocates nothing** (`memory.md` is the ledger). There is
no `new`, no container growth, no type erasure -- a record is `memcpy`ed
into a chunk this thread already holds. An allocator injected there would
have nothing to allocate. Worse, giving it a hook on that path would
reintroduce precisely what R1.2 forbids: a call site that can block,
fail, or take unbounded time inside somebody else's allocator.

So the two mechanisms are genuinely different, and conflating them is how
this feature turns into a performance regression with a configuration
surface.

### (a) On the hot path, "memory management" means *which channel*, not *malloc*

What a deep, performance-critical region actually wants is not a different
allocator. It wants its records to land somewhere of its choosing: a
buffer sized for it, not shared with the rest of the process, not
contending on the same claim cursor as every other subsystem, and not at
risk of being crowded out by a chatty one.

That is a **storage steering** decision, and the format already supports
it, because a channel is just another segment -- and merging segments at
read time is the design (`multi-process.md`). The read side needs nothing
new.

Sketch, per call site:

```cpp
// Static storage, sized for this region, claimed once. "Channel" is
// LTTng's word for exactly this and carries the right meaning already.
constinit sub0log::Channel<64 * 1024> cControlLoopChannel;

void controlLoopStep() {
    // Same macro family, same record, different destination.
    sub0log_debug_to(cControlLoopChannel, cControl, "step {} err {}", tick, error);
}
```

LTTng configures channels by sub-buffer size and count and states the cost
in the same place -- more memory -- so this should too, rather than
presenting a channel as free isolation.

What this buys, and why it is worth a distinct spelling:

- **No cross-subsystem contention.** The one atomic on the producer path
  is the chunk claim; a hot region with its own channel claims from its own
  cursor and never shares a cache line with unrelated code.
- **A bounded blast radius.** A region that floods cannot evict another
  subsystem's records, because it exhausts its own channel and counts its
  own drops. This is LTTng's documented reason for per-process buffering,
  arrived at from the same direction.
- **Locality worth having.** A 64 KiB channel touched by one loop stays
  warm in a way an 8 MiB shared segment does not.
- **It composes with the ladder.** A channel is a `MemorySegment`; the
  merged read view puts its records back in time order beside everything
  else.

The cost to be honest about, and LTTng states its equivalent in the same
place: a channel is more memory, and a second cursor is a second place
records can be lost. Its drop count is separate, so `Stats` must aggregate
-- a region silently running dry is exactly the invisible failure R9.1
exists to prevent.

### (b) Off the hot path, an allocator is the right tool -- and there is real work for it

The reason is not performance, and getting that wrong is how this feature
gets rejected. spdlog's maintainer turned down an allocator PR on the
grounds that spdlog allocates mainly at startup, which was true and did not
answer the requester, who needed every byte attributable to a console
memory budget regardless of how rarely it was spent. Budget accountability
is a separate requirement from latency, and it is the one this serves.

The places this library actually allocates are all cold, and all of them
are reasonable to hand to a consumer's allocator:

| site | allocates | who would care |
|---|---|---|
| `Decoder` | site table, per-record argument vectors | a decoding tool with a memory budget |
| `Merger` | the merged vector, one copy per record | merging many large segments |
| child capture | line buffers, command strings | a supervisor spawning many children |
| `Logger::create` | path and option strings | anyone, once, at startup |

Two different mechanisms suit two different sides, and the reason is
indirection:

- **Reader and merger: `std::pmr`.** They are cold, they already own
  containers, and a `memory_resource*` costs one indirection per
  allocation on a path measured in tens of nanoseconds per record. In
  exchange the API stays non-template, so `Decoder` does not become
  `Decoder<Alloc>` and infect every signature that mentions it. A tool
  wanting a bounded decode passes a `monotonic_buffer_resource` over a
  fixed array and gets exactly that.
- **Producer-side setup: a template policy.** `Logger::Options` holding
  `std::string` is the last `<string>` dependency on the producer side.
  A `StringPolicy`/fixed-capacity path type removes it for targets that
  cannot afford `<string>`, with zero indirection, and is invisible to
  everyone else because the default stays `std::string`.

### The rule that keeps this honest

> An allocator hook may never be reachable from a call site's emit path.

Setup, decode, merge and capture may allocate. The store sequence may not,
and no configuration should be able to make it. If a future backend needs
to allocate to accept a record, it belongs behind a rung that says so, the
way `MemorySegment` says it gives up durability.

## Suggested order

1. `ChunkSource` seam + `MemorySegment` (rung 1). Small, proves the seam,
   unlocks embedded and tests.
2. Per-call-site channels (`sub0log_*_to`) with aggregated `Stats`. Depends
   on 1 -- a channel *is* a `MemorySegment`.
3. `std::pmr` in `Decoder`/`Merger`. Independent, host-side, immediately
   useful to a bounded decoding tool.
4. String policy in `Logger::Options`. Last, smallest, only matters to
   targets that have already taken rungs 1 and 2.

Throughout, the usability constraint is not negotiable and has a concrete
failure mode to design against: quill's issue #609, where a component built
with custom options and one built with defaults would not interoperate. A
policy that escapes into the types an ordinary consumer reads has turned a
convenience into an interoperability problem. Default template argument,
`Logger` unchanged, policies invisible unless asked for, nothing global and
mutable.

Steps 1 and 2 are the ones that change what the library can do; 3 and 4
change what it costs. None of them touch the wire format, which is what
makes the whole ladder safe to build incrementally.
