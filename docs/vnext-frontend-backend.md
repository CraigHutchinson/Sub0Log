# vNext: a front-end / backend split, and where it can honestly go

A design consideration for the next version, recorded before anyone starts
refactoring toward it, because the obvious version of this idea quietly
gives away the library's central property. Nothing here is implemented; the
purpose is to fix *where the seam may go* so the eventual refactor starts
from the right place.

## The idea

Split the library into a front-end (what a call site touches) and a backend
(where records go), so that consumers can ladder up:

1. simple single-process logging with the smallest possible footprint;
2. the multi-process, merged-stream arrangement;
3. resilience (hard-kill durability, generations, recycling) as an explicit
   backend property rather than an always-on cost;
4. and examples shipping for each rung.

## The constraint the obvious split violates

The classic front-end/backend logger split -- call sites enqueue, a backend
thread dequeues and writes -- is exactly the staging arrangement
`hard-kill.md` exists to reject. A record in a front-end queue is
process-private memory; a kill takes it. R3.1's guarantee is *the absence
of a backend hop*: a record is durable the moment the producer's store
completes, because the store lands in kernel-owned memory.

So "resilience at the backend as an option" inverts the usual framing.
Resilience here is not something a backend adds; it is a property of which
transport the records land in. The split therefore cannot be drawn at a
record queue. It has to be drawn somewhere that leaves the per-record path
exactly as it is.

## Where the seam actually is

The code already has it, unnamed. The front-end -- macros, site
descriptors, encoding, correlation, the emit path -- never touches a file
or a mapping. It touches a `ChunkWriter`: a span it bump-allocates into and
commits against. Everything backend-ish happens at *chunk* granularity:
where the span's memory lives, how a fresh chunk is claimed, what the
segment header anchors it to.

That granularity is the whole trick. A backend abstraction at the chunk
source costs nothing per record, because backend code runs only on the cold
refill path (once per chunk, i.e. once per tens-to-hundreds of records).
A `ChunkSource` seam -- claim a chunk, describe the geometry, own the
anchor identity -- preserves R1 untouched and makes the backend a policy:

- **FileSegment** (today's behaviour, the default): file-backed shared
  mapping; hard-kill durable; mergeable across processes. Rungs 2 and 3.
- **MemorySegment**: an anonymous in-process buffer with the same layout.
  Loses R3 *by declared choice*; gains zero filesystem footprint. The
  right rung 1 for tools that want cheap structured logging and for tests
  and embedded uses -- and because the layout is identical, the same
  decoder reads it (dumped or inspected in-process).
- Later candidates that fit the same seam: segment rollover (v2 already),
  and an opt-in flush-on-fatal policy (`msync`/`FlushViewOfFile` when a
  `Fatal` record commits -- the one place a syscall per record is a price
  worth naming).

What must *not* vary per backend: the wire format, the commit-last
protocol, the descriptor split, and the encode rules. A backend chooses
where bytes live and how durable they are; it never chooses what the bytes
mean. One decoder reads all of them.

## Mechanism sketch (to be argued with, not followed blindly)

Compile-time policy first, virtual dispatch only if a real need appears:
`Logger` becomes `BasicLogger<ChunkSource>` with `Logger` an alias for
`BasicLogger<FileSegment>`, keeping today's API exactly. The scoped-active
binding stays on the alias; a test or example binds a
`BasicLogger<MemorySegment>` through the same seam (R7 unchanged). No
per-record indirection exists in either variant, and the disabled-site cost
(R1.4) is identical in both because the threshold check precedes any
backend involvement.

The examples ladder then falls out as `examples/` rungs 1-3 rather than as
documentation prose.

## What this does not change

R5's multi-process story stays a property of the FileSegment backend --
merging needs files. The C ABI (R4) fronts whatever backend the host bound,
unchanged. And the benchmark suite gains a `MemorySegment` variant of the
emit group, which will make the cost of the file mapping itself visible for
the first time (prediction: indistinguishable; the store is the store).

## Status

Deferred to vNext, after v1 stabilises. Recorded now so the refactor, when
it happens, is a rename of an existing seam rather than an invention -- and
so nobody reintroduces a record queue under the word "backend" without
meeting `hard-kill.md` first.
