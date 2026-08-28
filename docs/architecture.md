# Architecture

How the requirements in `REQUIREMENTS.md` become components, and the decisions
that were forced along the way. Each section names the requirements it
discharges; a decision that a requirement does not force is marked as a choice,
so it can be revisited without re-arguing the contract.

The reference material in this directory is the evidence base. Where a
mechanism below is borrowed, the borrowing is recorded in `record-model.md`,
`framing-and-recovery.md`, `hard-kill.md` and `multi-process.md` rather than
restated here.

## Language standard (R8.1, R8.2)

**C++23, unconditionally.** Every header compiles as C++23
(`target_compile_features(... cxx_std_23)`), and the tests build the same way.

The supporting facts, kept because they will be asked again: MSVC has no
`/std:c++26` yet, GCC grows `-std=c++26` only at GCC 14, and CMake's feature
table cannot request `cxx_std_26` from every toolchain the project calls
first-class -- so anything above C++23 today would demote platforms that R8.1
keeps first-class. When the toolchains catch up, moving the baseline is a
one-line CMake change plus a style-guide edit, and nothing in the format or
the API is shaped by the standard choice.

One library-availability note discovered the hard way: libstdc++ 13 withholds
`<expected>` from Clang < 19 (its guard wants a concepts feature-test value
Clang defines only from 19), so `std::expected` -- though C++23 on paper -- is
not usable across the first-class matrix. Fallible constructors therefore
return their object with `valid()` false and an `error()` beside it, which
also keeps the producer path exception-free.

## Component map

```
            producer side (hot)                      consumer side (cold)
  ┌────────────────────────────────────┐   ┌──────────────────────────────────┐
  │ log.hpp        call-site macros    │   │ reader.hpp   SegmentReader       │
  │ site.hpp       SiteDescriptor      │   │              Decoder             │
  │ encode.hpp     typed arg encoding  │   │ merge.hpp    Merger              │
  │ chunk.hpp      ChunkWriter         │   └──────────────────────────────────┘
  │ segment.hpp    Segment (mapping)   │                 ▲
  │ instance.hpp   Logger, ScopedBind  │                 │ reads files, never
  │ context.hpp    CorrelationScope    │                 │ talks to a producer
  └────────────────┬───────────────────┘                 │
                   │ both include                        │
        ┌──────────▼──────────────────────────────────────┴──────┐
        │ wire.hpp        the on-disk format: the one contract   │
        │ severity.hpp    the library-owned ladder               │
        └────────────────────────────────────────────────────────┘
        ┌────────────────────────────────────────────────────────┐
        │ detail/platform.hpp   file mapping + machine clock,    │
        │                       one interface, per-OS inside     │
        │ sub0log_abi.h         C ABI for plugins (R4)           │
        └────────────────────────────────────────────────────────┘
```

`wire.hpp` is the load-bearing header: producer and reader share nothing else.
A reader never touches producer state, in the same way the processes it merges
never touch each other (R5.1) -- the file is the whole interface.

The library is header-only (an `INTERFACE` CMake target). Nothing in the
producer path needs a translation unit, R8.3 forbids runtime dependencies
there anyway, and header-only is what makes "a plugin links nothing" (R4.1)
trivially true for in-process consumers. The C ABI shim is the one piece that
a host application compiles once and exports.

## The segment file (R3, R5.1)

One file per process per run, created in a shared directory, named
`<stem>-<pid>-<generation>.s0l`. Nothing is ever shared for writing
(`multi-process.md` has the argument). The file is created at full size and
mapped `MAP_SHARED` / `FILE_MAP_WRITE` over a real file handle -- the two
almost-right spellings that lose everything (`MAP_PRIVATE`, pagefile-backed
sections) are refused in `detail/platform.hpp`, and a test asserts the mapping
has a real backing file (`hard-kill.md`).

```
offset 0                                              segmentBytes
┌───────────────┬──────────┬──────────┬─────────────┬──────────┐
│ SegmentHeader │ chunk 0  │ chunk 1  │    ...      │ chunk N-1│
│  (one page)   │          │          │             │          │
└───────────────┴──────────┴──────────┴─────────────┴──────────┘
```

**SegmentHeader** (fixed 4 KiB page; layout in `wire.hpp`):

| field | why |
|---|---|
| `magic` (8 bytes, ends `0x1E`) | improbable, and a nod to RFC 7464's RS |
| `formatVersion` | **before** everything whose meaning could change -- the Kafka lesson (`framing-and-recovery.md`) |
| `headerBytes`, `chunkBytes`, `segmentBytes` | geometry is data, not convention; a reader trusts the file, not its own defaults |
| `generation` | random per segment; every chunk repeats it (R3.4) |
| `processId` | attribution for the merge |
| `anchorMono`, `anchorWall` | the anchor pair that makes cross-process merge sound (R5.3) |
| `nextChunk` (own cache line) | the single atomic claim cursor |

The anchor lives in the header rather than in a record: it exists once per
segment, is written before any record, and putting it at a fixed offset means
a merger can align segments without decoding anything. *(Choice, not forced --
an in-stream anchor record could be added later for re-anchoring after
suspend/resume.)*

## Chunks and the claim (R1.3, R3.1)

The body is fixed-size chunks (default 64 KiB, a segment-header value). A
thread owns at most one chunk at a time; claiming one is a single
`fetch_add(1, relaxed)` on `nextChunk` -- the only cross-thread synchronisation
on the producer path, and there is no cross-*process* synchronisation at all.

A claimed chunk begins with a `ChunkHeader`: the segment `generation` stamped
again (a reader rejects a stale chunk on positive evidence, R3.4), the owning
thread id, and the monotonic time at claim. Everything after it is written by
exactly one thread, unsynchronised (R1.3).

When the segment runs out of chunks, records are **dropped and counted**
(R9.1) -- never blocked on, never reallocated (R1.2). Segment rollover is a
later phase; the drop counter is not.

## Record framing (R3.3, R3.4)

Every record starts with **one 8-byte head word**, so validation touches one
word before any pointer moves (`framing-and-recovery.md`):

```
bits  0..15  payloadBytes   bounds-checked against the chunk before use
bits 16..23  kind           message / definition / continuation / blob
bits 24..31  flags          truncated, chain-continues
bits 32..47  sequence       per-chunk ordinal, torn-write evidence
bits 48..63  commitTag      constant 0xC511; anything else is not a record
```

**Commit is last.** The producer writes the payload, then release-stores the
head word. A reader acquire-loads it: a zero word is the unwritten remainder
of the chunk, a word with a wrong tag is a torn write; either way the reader
stops decoding *that chunk*, adds the remaining bytes to its
`unreadableBytes` count, and moves to the next chunk -- damage is bounded to a
chunk the way LevelDB bounds it to a block. There is no per-record CRC on the
default path: inside a mapped file the threat is an unfinished write, and
commit-last answers exactly that threat.

The payload length is bounds-checked against the chunk's remaining space
before a single payload byte is read. No length found in the file can send
the reader outside the chunk it was found in.

Recovery policy is *tolerate-corrupted-tail* per chunk, and the unreadable
byte count is part of the reader's result, not a side print (R3.3, R9.2).

## Records and the descriptor split (R1, R2, R4.3)

`record-model.md` records the reasoning; the shapes are:

**Message** -- `siteId` (the descriptor's address in the producing process),
monotonic timestamp, correlation id, then the argument bytes packed per the
site's type list. Fixed-size arguments are raw little-endian; string/bytes
arguments are a `u16` length then bytes, truncated at a cap with the
truncation flagged in the record (R9.2: visible, never silent).

**SiteDefinition** -- written once per site per process, *before* that site's
first message (strict self-description: everything needed to decode a record
precedes it). Carries `siteId`, severity, subsystem id, source file and line,
format string, and the argument type codes. The producer writes it; a reader
never resolves anything against a live module, so a plugin can unload and the
stream still decodes (R4.3).

The site's identity is its descriptor address: free, stable per process, no
lazy initialisation. It is only meaningful *within one segment*, which is
fine -- the definition record is what gives it meaning, and the decoder keys
its site table per segment. The announce flag beside the descriptor is a
constant-initialised atomic; the steady-state cost is one relaxed load and a
predictable branch.

**SubsystemDefinition** -- names one subsystem id, the same "definition
precedes use" discipline applied to the one axis it used to stop short of
(`record-model.md`, "The library must not own the vocabulary"). Written by
`Logger::create()` for names declared in `Options` and by
`Logger::declareSubsystem()` for one discovered later; control-thread work
only, never reachable from `detail::emit`. Adding it did not move
`wire::cFormatVersion`: `Decoder::decodeAll`'s first pass already routes any
`RecordKind` it does not recognise to `skippedRecords()` rather than
`undecodableRecords()`, so an older decoder reading a newer segment counts
these as intact-but-unrecognised, exactly as it already does for `Blob`.

**Continuation** -- a bounded chain for payloads that outgrow one record
(file paths). Capped; past the cap is truncation, flagged. *(Skeleton in v1:
the cap and flags are in the format from day one; the writer may truncate
without chaining until the chain is implemented.)*

**Blob** -- the cold bulk case. Allowed to allocate, because it runs on
threads that just spent milliseconds waiting (`record-model.md`). Deferred
past v1; the kind is reserved in the format now so adding it is not a version
bump.

## Argument encoding (R1.2, R2.1)

`encode.hpp` maps C++ argument types to wire type codes at compile time.
Anything that is not trivially copyable or a recognised view type **does not
compile** -- the refusal is the point (`record-model.md`). `std::string`
arguments are rejected with a static assertion that names the two choices
(inline the bytes via `std::string_view`, or log an identifier). Enums encode
as their underlying type. There is no fallback that formats, because a
fallback that formats is where the allocation comes back in.

## Call-site macros (R1.4)

`sub0log_trace/debug/info/warning/error/fatal(subsystem, fmt, ...)` --
lowercase by the style guide's argued exception. The macro:

1. loads the severity threshold (one relaxed load) and compares; on the
   disabled path **the arguments are never evaluated**;
2. materialises a distinct `constinit SiteDescriptor` with static storage
   duration (the two verified reasons this must be a macro are in
   `STYLE_GUIDE.md`);
3. calls `detail::emit(site, args...)`, which writes the definition record on
   first use and the message record always.

The subsystem argument is an opaque `SubsystemId` the consumer defines; the
library owns the severity ladder only, with `Unclassified` deliberately above
`Error` so no filter setting hides what the library could not classify
(R9.2, `record-model.md`).

## Instance and testability (R7)

`Logger` owns a segment and the counters. The active instance is a single
`constinit` atomic pointer -- no function-local static, no latched global.
`Logger::ScopedBind` binds an instance for a scope and restores the previous
one on exit, following the Sub0Pipeline scoped-active pattern; a test binds a
`Logger` over a temporary file, logs, unbinds, and reads the file back
synchronously. No test-only branch exists anywhere on the path (R7.2) -- the
seam is the binding, and it is the same seam production uses at startup.

With no instance bound, call sites cost the threshold load and nothing else.

## Correlation (R5.4, R6)

A `u64` correlation id in a `thread_local`, set by `CorrelationScope` (RAII,
saves and restores), stamped into every message record. Propagation into
children is by environment variable (`SUB0LOG_CORRELATION`), because that is
the mechanism that survives `exec` of an unmodified binary; a `Logger` seeds
its root correlation from that variable when present, and the emit path
falls back to that root whenever no scope is active on the calling thread --
so an unmodified cooperating binary joins the activity that spawned it with
no wiring code. The root lives on the instance rather than in a process-wide
latch, so a test reaches it through the same scoped binding as everything
else (R7).

## Child processes: capture and interception (R5.5, R5.6)

A third-party executable will not write records in this format no matter what
it is handed, so its output streams are the whole interface. `child.hpp`
gives the parent one primitive:

`ChildProcess::spawn(options)` pipes the child's stdout/stderr, writes a
**ChildStart** record (command line, OS pid, the correlation id in scope at
spawn), and runs one capture thread per stream. Each complete line becomes a
**ChildOutput** record; `wait()` drains the pipes to EOF, reaps, and writes
**ChildExit** (exit code or signal). The three payloads join on a parent-side
`childId` ordinal rather than the pid, because pids recycle; a line's
correlation is reached through its ChildStart, an equality join (R6.2),
rather than repeating eight bytes per captured line.

Attribution answers R5.5 in full: what was run, what it printed, how it
ended, and which activity spawned it -- with `SUB0LOG_CORRELATION` also
stamped into the child's environment so a *cooperating* descendant joins the
same activity through R5.4, and a non-cooperating one simply ignores it.

**Interception (R5.6)** rides the same path because the path can afford it:
capture threads run at pipe-buffer pace, milliseconds not nanoseconds, so a
caller-supplied callback there costs nothing that matters. The registered
`LineInterceptor` sees each line before its record is written and returns Log
or Suppress -- enough to detect a readiness message, harvest a value from
output, or drop noise at source. Suppressions are counted (R9.1 treats
deliberate suppression like a drop: visible), and an exception escaping the
interceptor logs the line anyway -- capture must not die because a matcher
did. The same hook is deliberately *not* offered on the in-process emit path:
a callback there would reintroduce exactly the unbounded producer work R1
forbids, and R5.6 now rules it out in writing.

The capture machinery is allowed to allocate (line buffers, the threads) --
this is the cold path the blob argument in `record-model.md` describes.
Over-long lines are split at the record cap with the truncation flagged.

## Reader, decoder, merger (R2, R3.3, R5.2)

- **`SegmentReader`**: maps or is handed bytes, validates the header, walks
  chunks, yields raw committed records, counts what it could not interpret.
  Never trusts a length it has not bounds-checked; never trusts a chunk whose
  generation is not the segment's.
- **`Decoder`**: builds the site table from definition records as they
  arrive, turns raw records into typed `DecodedRecord`s (`std::variant`
  arguments -- an integer arrives as an integer, R2.1), filters by
  subsystem/severity/time/thread/correlation without touching message text
  (R2.2), and only formats (`std::format`, the site's format string) when a
  consumer asks for text.
- **`Merger`**: N-way merge of decoded segments ordered by
  `anchorWall + (mono - anchorMono)` per segment -- the anchor arithmetic
  from `multi-process.md`. Ordering policy lives here, at read time, by
  design (R5.2).

The decoder ships as a library first; a CLI (`tools/`) is a thin consumer of
it and comes later -- this answers the third open question in
`REQUIREMENTS.md` with "library, then CLI on top".

## The C ABI (R4)

`sub0log_abi.h` is C, dependency-free, and carries a versioned function table
(`Sub0LogAbiV1`: `define_site` / `emit` / `current_correlation`) that a host
exports to its plugins with one exported symbol
(`SUB0LOG_ABI_GETTER_NAME`, `sub0log_abi_v1`). A plugin compiles the header
only; it never links the library (R4.1), and nothing crossing the boundary
owns memory or runs C++ (R4.2). The table is `size`-versioned so it can grow
without breaking old plugins.

`abi_host.hpp` is the host-side implementation, over the same producer path
`detail::emit` uses: `define_site` writes a SiteDefinition through
`detail::writeSiteDefinitionCore` (the non-template core `writeSiteDefinition`
now also calls, so the two paths share one record layout rather than two);
`emit` writes a Message from the plugin's pre-encoded payload through
`detail::reserveRecord`, stamping time and correlation the same way
`detail::emit` does. A plugin's call site has no SiteDescriptor to hang
`announcedGeneration_` off (site.hpp), so the host tracks, in a table keyed
on the plugin's site id, which segment generation last received that site's
definition; `emit` refuses (and counts, R9.1) a Message for a site this
segment was never told about, rather than trust the plugin re-announced in
time -- the same failure the generation keying already forbids for the C++
path, extended across the boundary. `SUB0LOG_ABI_HOST_EXPORT()` is the macro
a host writes once, in one translation unit, to define and export
`sub0log_abi_v1` with default visibility; the exported getter returns NULL
with no Logger bound, as documented in the header.

`tests/system/abi.test.cpp` is the round trip: a plugin
(`tests/system/plugin/abi_test_plugin.cpp`) built `-fvisibility=hidden` --
docs/adoption-friction.md 2.3's exact configuration -- including only
`sub0log_abi.h` and linking nothing of the library, `dlopen()`ed, handed the
table, logs, `dlclose()`ed, and only then decoded: R4.1 (no duplicated
instance) and R4.3 (decodable after unload) both pinned against a real
loader rather than argued.

## Dependencies and build

CPM (`cmake/CPM.cmake`, vendored) manages packages. The producer path has
none (R8.3) and will keep none; **doctest** is fetched for tests only, pinned
by version and hash, and never installed or exported. `SUB0LOG_BUILD_TESTING`
gates all of it, so a consumer embedding the library via
`add_subdirectory`/CPM pays for nothing.

## Phasing

- **v1 (this branch)**: format, producer path (POSIX mapping; Windows behind
  the platform interface, compiled but CI-unverified), reader/decoder/merger,
  scoped binding, correlation, drop counters, tests including a
  producer-round-trip and a kill-the-writer recovery test.
- **v1 stretch**: child capture and interception (R5.5/R5.6) -- format and
  API are in v1 (`child.hpp`, the three Child* payloads); the POSIX
  implementation lands as this branch's stretch goal.
- **v2 (complete)**: the dlopen ABI round trip (`abi_host.hpp`,
  `tests/system/abi.test.cpp`), which closed R4 and with it the last open
  finding in `adoption-friction.md`; continuation chains, so a Bytes
  argument reaches 4096 bytes rather than being cut at 512; and the Windows
  child-capture path. The Windows work is no longer "written but
  CI-unverified" -- `windows-msvc` builds and runs both the ABI round trip
  and seven of the nine process-spawning child tests, the two exceptions
  being cases Windows cannot be asked (there is no signal to send a child,
  and a bad executable path fails in `spawn()` rather than in the child).
  The blob channel was reviewed and deliberately not built; the reasoning
  is in `record-model.md` beside the mechanism it describes.
- **v3**: segment rollover. The CLI arrived early -- `tools/sub0log-cat`
  shipped in v1, because `adoption-friction.md` 2.1 found that every
  consumer, this repository's own examples included, was writing the same
  forty-line printer before they could see anything at all.
- **vNext**: a front-end/backend split at the chunk-source seam.
  `vnext-frontend-backend.md` fixes where that seam may go and why the
  classic queue-based split is off the table;
  `vnext-backends-and-memory.md` carries it forward into the ladder's rungs
  (naive `MemorySegment` first, today's `FileSegment` above it),
  per-call-site arenas for hot regions, and `std::pmr` on the reader --
  with one rule holding it together: an allocator hook may never be
  reachable from a call site's emit path.

## Answers to the open questions in REQUIREMENTS.md

- **Descriptor addressed by pointer or content id?** Pointer, scoped to the
  segment by its definition record. A content id buys cross-segment site
  dedup at the cost of hashing on the producer; nothing in R1–R9 wants that
  trade.
- **Chunk size?** A per-segment header value, default 64 KiB. High-rate and
  occasional producers differ by *configuration*, not by format.
- **Decoder: library or CLI?** Library; CLI later, on top.
